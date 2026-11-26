#include "video/D3D11Renderer.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

#include <algorithm>
#include <cstring>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
}

namespace dexlink {

namespace {

template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

}  // namespace

D3D11Renderer::D3D11Renderer(SharedTelemetry& telemetry) : telemetry_(telemetry) {}

D3D11Renderer::~D3D11Renderer() {
    Shutdown();
}

bool D3D11Renderer::Initialize(SDL_Window* window, int width, int height) {
    SDL_SysWMinfo wmInfo{};
    SDL_VERSION(&wmInfo.version);
    if (!SDL_GetWindowWMInfo(window, &wmInfo)) {
        return false;
    }
    HWND hwnd = wmInfo.info.win.window;
    target_w_ = std::max(width, 1);
    target_h_ = std::max(height, 1);

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL achieved{};

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels,
        2,
        D3D11_SDK_VERSION,
        &device_,
        &achieved,
        &context_
    );
    if (FAILED(hr)) {
        flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            levels,
            2,
            D3D11_SDK_VERSION,
            &device_,
            &achieved,
            &context_
        );
    }
    if (FAILED(hr) || !device_ || !context_) {
        return false;
    }

    if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&multithread_)))) {
        multithread_->SetMultithreadProtected(TRUE);
    }

    IDXGIDevice* dxgi_dev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory2 = nullptr;
    if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&dxgi_dev))) ||
        FAILED(dxgi_dev->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory2)))) {
        SafeRelease(factory2);
        SafeRelease(adapter);
        SafeRelease(dxgi_dev);
        return false;
    }

    IDXGIFactory5* factory5 = nullptr;
    if (SUCCEEDED(factory2->QueryInterface(IID_PPV_ARGS(&factory5)))) {
        BOOL tearing = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) {
            allow_tearing_ = (tearing == TRUE);
        }
        SafeRelease(factory5);
    }

    DXGI_SWAP_CHAIN_DESC1 sc_desc{};
    sc_desc.Width = static_cast<UINT>(target_w_);
    sc_desc.Height = static_cast<UINT>(target_h_);
    sc_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = 2;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc_desc.Flags = allow_tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    hr = factory2->CreateSwapChainForHwnd(device_, hwnd, &sc_desc, nullptr, nullptr, &swapchain_);
    factory2->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    SafeRelease(factory2);
    SafeRelease(adapter);
    SafeRelease(dxgi_dev);

    if (FAILED(hr) || !swapchain_) {
        return false;
    }

    // Acquire VideoDevice / VideoContext for zero-copy D3D11VA NV12->BGRA GPU blits
    if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&video_dev_))) ||
        FAILED(context_->QueryInterface(IID_PPV_ARGS(&video_ctx_)))) {
        SafeRelease(video_dev_);
        SafeRelease(video_ctx_);
    }

    return EnsureRenderTarget();
}

void D3D11Renderer::Shutdown() {
    ReleaseVideoResources();
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    if (sw_transfer_frame_) {
        av_frame_free(&sw_transfer_frame_);
    }
    SafeRelease(rtv_);
    SafeRelease(swapchain_);
    SafeRelease(video_ctx_);
    SafeRelease(video_dev_);
    SafeRelease(multithread_);
    SafeRelease(context_);
    SafeRelease(device_);
}

bool D3D11Renderer::EnsureRenderTarget() {
    SafeRelease(rtv_);
    if (!swapchain_ || !device_) return false;
    ID3D11Texture2D* back_buf = nullptr;
    HRESULT hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(&back_buf));
    if (FAILED(hr) || !back_buf) return false;
    hr = device_->CreateRenderTargetView(back_buf, nullptr, &rtv_);
    SafeRelease(back_buf);
    return SUCCEEDED(hr);
}

void D3D11Renderer::Resize(int width, int height) {
    if (!swapchain_ || width <= 0 || height <= 0) return;
    if (width == target_w_ && height == target_h_) return;
    target_w_ = width;
    target_h_ = height;
    SafeRelease(rtv_);
    UINT flags = allow_tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    swapchain_->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, flags);
    EnsureRenderTarget();
}

void D3D11Renderer::BeginFrame() {
    if (!context_ || !rtv_) return;
    const float clear_col[4] = {0.051f, 0.051f, 0.051f, 1.0f}; // #0D0D0D
    context_->OMSetRenderTargets(1, &rtv_, nullptr);
    context_->ClearRenderTargetView(rtv_, clear_col);
}

void D3D11Renderer::EndFrame(bool vsync) {
    if (!swapchain_) return;
    if (vsync) {
        swapchain_->Present(1, 0);
    } else {
        UINT flags = allow_tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0;
        swapchain_->Present(0, flags);
    }
}

void D3D11Renderer::ReleaseVideoResources() {
    SafeRelease(vp_out_view_);
    SafeRelease(vp_);
    SafeRelease(vp_enum_);
    SafeRelease(video_srv_);
    SafeRelease(staging_tex_);
    SafeRelease(video_tex_);
    video_w_ = 0;
    video_h_ = 0;
    vp_src_w_ = 0;
    vp_src_h_ = 0;
    tex_is_dynamic_ = false;
}

bool D3D11Renderer::EnsureVideoOutputTexture(int w, int h) {
    if (video_tex_ && video_srv_ && video_w_ == w && video_h_ == h && !tex_is_dynamic_) {
        return true;
    }
    ReleaseVideoResources();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    if (FAILED(device_->CreateTexture2D(&td, nullptr, &video_tex_))) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    if (FAILED(device_->CreateShaderResourceView(video_tex_, &srvd, &video_srv_))) {
        ReleaseVideoResources();
        return false;
    }

    video_w_ = w;
    video_h_ = h;
    tex_is_dynamic_ = false;
    return true;
}

bool D3D11Renderer::EnsureVideoProcessor(int src_w, int src_h, int dst_w, int dst_h) {
    if (!video_dev_ || !video_ctx_) return false;
    if (!EnsureVideoOutputTexture(dst_w, dst_h)) return false;

    if (vp_ && vp_out_view_ && vp_src_w_ == src_w && vp_src_h_ == src_h) {
        return true;
    }

    SafeRelease(vp_out_view_);
    SafeRelease(vp_);
    SafeRelease(vp_enum_);

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = static_cast<UINT>(src_w);
    content.InputHeight = static_cast<UINT>(src_h);
    content.OutputWidth = static_cast<UINT>(dst_w);
    content.OutputHeight = static_cast<UINT>(dst_h);
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    if (FAILED(video_dev_->CreateVideoProcessorEnumerator(&content, &vp_enum_)) || !vp_enum_) {
        return false;
    }
    if (FAILED(video_dev_->CreateVideoProcessor(vp_enum_, 0, &vp_)) || !vp_) {
        SafeRelease(vp_enum_);
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovd.Texture2D.MipSlice = 0;
    if (FAILED(video_dev_->CreateVideoProcessorOutputView(video_tex_, vp_enum_, &ovd, &vp_out_view_)) || !vp_out_view_) {
        SafeRelease(vp_);
        SafeRelease(vp_enum_);
        return false;
    }

    vp_src_w_ = src_w;
    vp_src_h_ = src_h;
    return true;
}

bool D3D11Renderer::UploadD3D11Frame(AVFrame* frame) {
    auto* src_tex = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    const UINT slice = static_cast<UINT>(reinterpret_cast<uintptr_t>(frame->data[1]));
    if (!src_tex) return false;

    D3D11_TEXTURE2D_DESC src_desc{};
    src_tex->GetDesc(&src_desc);

    if (EnsureVideoProcessor(
            static_cast<int>(src_desc.Width),
            static_cast<int>(src_desc.Height),
            frame->width,
            frame->height)) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.FourCC = 0;
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.MipSlice = 0;
        ivd.Texture2D.ArraySlice = slice;

        ID3D11VideoProcessorInputView* in_view = nullptr;
        if (SUCCEEDED(video_dev_->CreateVideoProcessorInputView(src_tex, vp_enum_, &ivd, &in_view)) && in_view) {
            RECT src_rect{0, 0, frame->width, frame->height};
            RECT dst_rect{0, 0, frame->width, frame->height};
            video_ctx_->VideoProcessorSetStreamSourceRect(vp_, 0, TRUE, &src_rect);
            video_ctx_->VideoProcessorSetStreamDestRect(vp_, 0, TRUE, &dst_rect);
            video_ctx_->VideoProcessorSetOutputTargetRect(vp_, TRUE, &dst_rect);

            D3D11_VIDEO_PROCESSOR_STREAM stream{};
            stream.Enable = TRUE;
            stream.pInputSurface = in_view;

            HRESULT hr = video_ctx_->VideoProcessorBlt(vp_, vp_out_view_, 0, 1, &stream);
            in_view->Release();
            if (SUCCEEDED(hr)) {
                return true;
            }
        }
    }

    // Fallback: transfer D3D11 hardware frame to CPU and upload BGRA
    if (!sw_transfer_frame_) {
        sw_transfer_frame_ = av_frame_alloc();
    }
    av_frame_unref(sw_transfer_frame_);
    if (av_hwframe_transfer_data(sw_transfer_frame_, frame, 0) == 0) {
        return UploadSoftwareFrame(sw_transfer_frame_);
    }
    return false;
}

bool D3D11Renderer::UploadSoftwareFrame(AVFrame* frame) {
    if (frame->width <= 0 || frame->height <= 0) return false;

    if (!video_tex_ || !video_srv_ || video_w_ != frame->width || video_h_ != frame->height || !tex_is_dynamic_) {
        ReleaseVideoResources();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(frame->width);
        td.Height = static_cast<UINT>(frame->height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        if (FAILED(device_->CreateTexture2D(&td, nullptr, &video_tex_))) {
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipLevels = 1;
        if (FAILED(device_->CreateShaderResourceView(video_tex_, &srvd, &video_srv_))) {
            ReleaseVideoResources();
            return false;
        }
        video_w_ = frame->width;
        video_h_ = frame->height;
        tex_is_dynamic_ = true;
    }

    if (!sws_ctx_ || sws_src_w_ != frame->width || sws_src_h_ != frame->height || sws_src_fmt_ != frame->format) {
        if (sws_ctx_) sws_freeContext(sws_ctx_);
        sws_ctx_ = sws_getContext(
            frame->width,
            frame->height,
            static_cast<AVPixelFormat>(frame->format),
            frame->width,
            frame->height,
            AV_PIX_FMT_BGRA,
            SWS_FAST_BILINEAR,
            nullptr,
            nullptr,
            nullptr
        );
        sws_src_w_ = frame->width;
        sws_src_h_ = frame->height;
        sws_src_fmt_ = frame->format;
    }
    if (!sws_ctx_) return false;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(video_tex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }

    uint8_t* dst_slices[4] = {static_cast<uint8_t*>(mapped.pData), nullptr, nullptr, nullptr};
    int dst_linesize[4] = {static_cast<int>(mapped.RowPitch), 0, 0, 0};
    sws_scale(sws_ctx_, frame->data, frame->linesize, 0, frame->height, dst_slices, dst_linesize);

    context_->Unmap(video_tex_, 0);
    return true;
}

bool D3D11Renderer::UpdateVideoTextureFromSlot(FrameSlot& slot) {
    AVFrame* frame = slot.PopLatest();
    if (!frame) return false;

    bool ok = false;
    if (frame->format == AV_PIX_FMT_D3D11) {
        ok = UploadD3D11Frame(frame);
    } else {
        ok = UploadSoftwareFrame(frame);
    }

    if (ok) {
        const int vw = frame->width;
        const int vh = frame->height;
        const uint64_t dropped = slot.DroppedCount();
        telemetry_.Update([vw, vh, dropped](TelemetrySnapshot& s) {
            s.video_width = vw;
            s.video_height = vh;
            s.dropped_frames = dropped;
        });
    }

    av_frame_free(&frame);
    return ok;
}

}  // namespace dexlink
