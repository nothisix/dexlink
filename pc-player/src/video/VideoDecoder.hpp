#pragma once

#include "core/Config.hpp"
#include "dexlink_protocol.hpp"
#include "video/D3D11Renderer.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
}

namespace dexlink {

class VideoDecoder {
public:
    VideoDecoder(SharedTelemetry& telemetry, FrameSlot& output_slot);
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool Start(ID3D11Device* d3d_device, ID3D11DeviceContext* d3d_context);
    void Stop();

    void PushNalPacket(uint8_t flags, uint32_t ts_ms, const uint8_t* data, size_t len);

private:
    struct NalPacket {
        uint8_t flags = 0;
        uint32_t ts_ms = 0;
        std::vector<uint8_t> data;
    };

    void DecodeLoop();
    bool InitCodec(protocol::VideoCodec codec_type);
    void ShutdownCodec();

    static enum AVPixelFormat SelectD3D11Format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts);

    SharedTelemetry& telemetry_;
    FrameSlot& output_slot_;

    ID3D11Device* d3d_device_ = nullptr;
    ID3D11DeviceContext* d3d_context_ = nullptr;
    AVBufferRef* hw_device_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    protocol::VideoCodec active_codec_ = protocol::VideoCodec::H264;
    bool hw_enabled_ = false;

    std::vector<uint8_t> cached_config_;
    bool waiting_for_keyframe_ = true;

    std::atomic<bool> running_{false};
    std::thread decode_thread_;
    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::deque<NalPacket> queue_;
};

}  // namespace dexlink
