#include "video/VideoDecoder.hpp"

#include <chrono>
#include <cstring>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

namespace dexlink {

VideoDecoder::VideoDecoder(SharedTelemetry& telemetry, FrameSlot& output_slot)
    : telemetry_(telemetry), output_slot_(output_slot) {}

VideoDecoder::~VideoDecoder() {
    Stop();
}

enum AVPixelFormat VideoDecoder::SelectD3D11Format(AVCodecContext* /*ctx*/, const enum AVPixelFormat* pix_fmts) {
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_D3D11) {
            return *p;
        }
    }
    // Fallback to first supported software format
    return pix_fmts[0];
}

bool VideoDecoder::Start(ID3D11Device* d3d_device, ID3D11DeviceContext* d3d_context) {
    if (running_.exchange(true)) return true;
    d3d_device_ = d3d_device;
    d3d_context_ = d3d_context;

    if (d3d_device_ && d3d_context_) {
        AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (ref) {
            auto* hw_dev = reinterpret_cast<AVHWDeviceContext*>(ref->data);
            auto* d3d11_hw = reinterpret_cast<AVD3D11VADeviceContext*>(hw_dev->hwctx);
            d3d_device_->AddRef();
            d3d_context_->AddRef();
            d3d11_hw->device = d3d_device_;
            d3d11_hw->device_context = d3d_context_;

            if (av_hwdevice_ctx_init(ref) == 0) {
                hw_device_ctx_ = ref;
            } else {
                av_buffer_unref(&ref);
            }
        }
    }

    if (!InitCodec(protocol::VideoCodec::H264)) {
        Stop();
        return false;
    }

    decode_thread_ = std::thread(&VideoDecoder::DecodeLoop, this);
    return true;
}

void VideoDecoder::Stop() {
    if (!running_.exchange(false)) return;
    queue_cv_.notify_all();
    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        queue_.clear();
    }
    ShutdownCodec();
    if (hw_device_ctx_) {
        av_buffer_unref(&hw_device_ctx_);
    }
}

bool VideoDecoder::InitCodec(protocol::VideoCodec codec_type) {
    ShutdownCodec();

    const AVCodecID id = (codec_type == protocol::VideoCodec::Hevc) ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    const AVCodec* codec = avcodec_find_decoder(id);
    if (!codec) return false;

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) return false;

    // Zero-latency decoding: single-thread decode eliminates frame-reorder delay
    codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
    codec_ctx_->thread_count = 1;

    if (hw_device_ctx_) {
        codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
        codec_ctx_->get_format = SelectD3D11Format;
        codec_ctx_->extra_hw_frames = 4;
        hw_enabled_ = true;
    } else {
        hw_enabled_ = false;
    }

    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        // Retry software-only if D3D11VA hardware open failed
        if (hw_enabled_) {
            avcodec_free_context(&codec_ctx_);
            codec_ctx_ = avcodec_alloc_context3(codec);
            if (!codec_ctx_) return false;
            codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
            codec_ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
            codec_ctx_->thread_count = 1;
            hw_enabled_ = false;
            if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
                avcodec_free_context(&codec_ctx_);
                return false;
            }
        } else {
            avcodec_free_context(&codec_ctx_);
            return false;
        }
    }

    active_codec_ = codec_type;
    waiting_for_keyframe_ = true;
    return true;
}

void VideoDecoder::ShutdownCodec() {
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
    }
    cached_config_.clear();
}

void VideoDecoder::PushNalPacket(uint8_t flags, uint32_t ts_ms, const uint8_t* data, size_t len) {
    if (!running_.load() || !data || len == 0) return;

    NalPacket pkt;
    pkt.flags = flags;
    pkt.ts_ms = ts_ms;
    pkt.data.assign(data, data + len);

    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        // Safety bound against pathological stall: never accumulate stale GOPs across seconds
        if (queue_.size() > 120) {
            queue_.clear();
            waiting_for_keyframe_ = true;
        }
        queue_.push_back(std::move(pkt));
    }
    queue_cv_.notify_one();
}

void VideoDecoder::DecodeLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    AVPacket* avpkt = av_packet_alloc();
    AVFrame* decoded = av_frame_alloc();
    int fps_count = 0;
    auto fps_start = std::chrono::steady_clock::now();

    while (running_.load()) {
        NalPacket pkt;
        {
            std::unique_lock<std::mutex> lock(queue_mu_);
            queue_cv_.wait(lock, [this] { return !running_.load() || !queue_.empty(); });
            if (!running_.load()) break;
            pkt = std::move(queue_.front());
            queue_.pop_front();
        }

        const auto pkt_codec = (pkt.flags & protocol::kVideoFlagHevc)
                                   ? protocol::VideoCodec::Hevc
                                   : protocol::VideoCodec::H264;
        if (pkt_codec != active_codec_) {
            InitCodec(pkt_codec);
        }
        if (!codec_ctx_) continue;

        const bool is_config = (pkt.flags & protocol::kVideoFlagConfig) != 0;
        const bool is_keyframe = (pkt.flags & protocol::kVideoFlagKeyframe) != 0;

        if (is_config) {
            if (!cached_config_.empty() && cached_config_ != pkt.data) {
                avcodec_flush_buffers(codec_ctx_);
                waiting_for_keyframe_ = true;
            }
            cached_config_ = pkt.data;
            // Send config immediately so decoder prepares SPS/PPS before IDR
        } else if (waiting_for_keyframe_ && !is_keyframe) {
            continue;
        }

        if (is_keyframe) {
            waiting_for_keyframe_ = false;
        }

        std::vector<uint8_t> combined;
        const uint8_t* feed_ptr = pkt.data.data();
        size_t feed_len = pkt.data.size();

        // Prepend cached SPS/PPS if IDR didn't include config start bytes
        if (is_keyframe && !cached_config_.empty() &&
            (pkt.data.size() < cached_config_.size() ||
             std::memcmp(pkt.data.data(), cached_config_.data(), cached_config_.size()) != 0)) {
            combined.resize(cached_config_.size() + pkt.data.size());
            std::memcpy(combined.data(), cached_config_.data(), cached_config_.size());
            std::memcpy(combined.data() + cached_config_.size(), pkt.data.data(), pkt.data.size());
            feed_ptr = combined.data();
            feed_len = combined.size();
        }

        if (av_new_packet(avpkt, static_cast<int>(feed_len)) < 0) {
            continue;
        }
        std::memcpy(avpkt->data, feed_ptr, feed_len);
        avpkt->pts = static_cast<int64_t>(pkt.ts_ms);
        avpkt->dts = avpkt->pts;

        int ret = avcodec_send_packet(codec_ctx_, avpkt);
        av_packet_unref(avpkt);
        if (ret < 0) {
            continue;
        }

        while (true) {
            ret = avcodec_receive_frame(codec_ctx_, decoded);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF || ret < 0) {
                break;
            }

            AVFrame* handed = av_frame_alloc();
            av_frame_move_ref(handed, decoded);
            // 1-slot Drop-Oldest Queue: discard any unpresented older frame
            output_slot_.PushDropOldest(handed);

            fps_count++;
            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - fps_start).count();
            if (elapsed_ms >= 500) {
                float dec_fps = fps_count * 1000.0f / static_cast<float>(elapsed_ms);
                telemetry_.Update([dec_fps](TelemetrySnapshot& s) { s.dec_fps = dec_fps; });
                fps_count = 0;
                fps_start = now;
            }
        }
    }

    av_frame_free(&decoded);
    av_packet_free(&avpkt);
}

}  // namespace dexlink
