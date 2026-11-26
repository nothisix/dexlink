#pragma once

#include "core/Config.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

struct SDL_Window;

namespace dexlink {

// 1-slot Drop-Oldest Queue: eliminates latency drift and micro-stutter.
struct FrameSlot {
    std::mutex mu;
    AVFrame* latest = nullptr;
    uint64_t sequence = 0;
    uint64_t dropped = 0;

    void PushDropOldest(AVFrame* frame) {
        std::lock_guard<std::mutex> lock(mu);
        if (latest != nullptr) {
            av_frame_free(&latest);
            dropped++;
        }
        latest = frame;
        sequence++;
    }

    AVFrame* PopLatest() {
        std::lock_guard<std::mutex> lock(mu);
        AVFrame* out = latest;
        latest = nullptr;
        return out;
    }

    uint64_t DroppedCount() {
        std::lock_guard<std::mutex> lock(mu);
        return dropped;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mu);
        if (latest != nullptr) {
            av_frame_free(&latest);
        }
    }
};

class D3D11Renderer {
public:
    explicit D3D11Renderer(SharedTelemetry& telemetry);
    ~D3D11Renderer();

    D3D11Renderer(const D3D11Renderer&) = delete;
    D3D11Renderer& operator=(const D3D11Renderer&) = delete;

    bool Initialize(SDL_Window* window, int width, int height);
    void Shutdown();
    void Resize(int width, int height);

    void BeginFrame();
    void EndFrame(bool vsync);

    // Consumes at most 1 latest decoded frame (if available) and updates video_srv_
    bool UpdateVideoTextureFromSlot(FrameSlot& slot);

    ID3D11Device* Device() const { return device_; }
    ID3D11DeviceContext* Context() const { return context_; }
    ID3D11ShaderResourceView* VideoSrv() const { return video_srv_; }
    int VideoWidth() const { return video_w_; }
    int VideoHeight() const { return video_h_; }

private:
    bool EnsureRenderTarget();
    bool EnsureVideoOutputTexture(int w, int h);
    bool EnsureVideoProcessor(int src_w, int src_h, int dst_w, int dst_h);
    bool UploadD3D11Frame(AVFrame* frame);
    bool UploadSoftwareFrame(AVFrame* frame);
    void ReleaseVideoResources();

    SharedTelemetry& telemetry_;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11Multithread* multithread_ = nullptr;
    IDXGISwapChain1* swapchain_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    bool allow_tearing_ = false;
    int target_w_ = 1280;
    int target_h_ = 720;

    // GPU zero-copy VideoProcessor path (D3D11VA NV12 -> BGRA SRV)
    ID3D11VideoDevice* video_dev_ = nullptr;
    ID3D11VideoContext* video_ctx_ = nullptr;
    ID3D11VideoProcessorEnumerator* vp_enum_ = nullptr;
    ID3D11VideoProcessor* vp_ = nullptr;
    ID3D11VideoProcessorOutputView* vp_out_view_ = nullptr;
    int vp_src_w_ = 0;
    int vp_src_h_ = 0;

    // Shared renderable video texture (SRV for ImGui viewport)
    ID3D11Texture2D* video_tex_ = nullptr;
    ID3D11Texture2D* staging_tex_ = nullptr;
    ID3D11ShaderResourceView* video_srv_ = nullptr;
    int video_w_ = 0;
    int video_h_ = 0;
    bool tex_is_dynamic_ = false;

    // Software fallback swscale
    SwsContext* sws_ctx_ = nullptr;
    int sws_src_w_ = 0;
    int sws_src_h_ = 0;
    int sws_src_fmt_ = -1;
    AVFrame* sw_transfer_frame_ = nullptr;
};

}  // namespace dexlink
