#pragma once

#include "adb/AdbManager.hpp"
#include "core/Config.hpp"
#include "input/Keymapper.hpp"
#include "net/UsbTransport.hpp"
#include "video/D3D11Renderer.hpp"

#include <SDL2/SDL.h>
#include <imgui.h>

#include <string>
#include <vector>

namespace dexlink {

class DexlinkUi {
public:
    DexlinkUi(
        AppConfig& cfg,
        SharedTelemetry& telemetry,
        AdbManager& adb,
        UsbTransport& transport,
        Keymapper& keymapper
    );

    void InitFontsAndStyle();
    void Render(
        SDL_Window* window,
        D3D11Renderer& renderer,
        VideoRect& out_video_rect,
        bool& out_quit_requested
    );

    bool IsDraggingKeymap() const { return dragging_node_ != DragTarget::None || waiting_rebind_; }
    bool HandleKeymapEditorEvent(const SDL_Event& ev, const VideoRect& vrect);

private:
    enum class DragTarget {
        None,
        JoystickCenter,
        JoystickRadius,
        MouseLookCenter,
        TapNode
    };

    void RenderTopBar(SDL_Window* window, const TelemetrySnapshot& tel, bool& quit_requested);
    void RenderRightDrawer(const TelemetrySnapshot& tel);
    void RenderKeymapOverlay(const VideoRect& vrect);

    AppConfig& cfg_;
    SharedTelemetry& telemetry_;
    AdbManager& adb_;
    UsbTransport& transport_;
    Keymapper& keymapper_;

    ImFont* font_ui_ = nullptr;
    ImFont* font_mono_ = nullptr;

    std::vector<std::string> profiles_;
    char launch_pkg_buf_[128] = "com.tencent.ig";

    DragTarget dragging_node_ = DragTarget::None;
    int selected_tap_idx_ = -1;
    bool waiting_rebind_ = false;
    bool dragging_window_ = false;
    int drag_start_wx_ = 0;
    int drag_start_wy_ = 0;
    int drag_start_mx_ = 0;
    int drag_start_my_ = 0;
};

}  // namespace dexlink
