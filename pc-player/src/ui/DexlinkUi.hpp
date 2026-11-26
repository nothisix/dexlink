#pragma once

#include "adb/AdbManager.hpp"
#include "audio/WasapiAudioPlayer.hpp"
#include "core/Config.hpp"
#include "input/Keymapper.hpp"
#include "net/UsbTransport.hpp"
#include "video/D3D11Renderer.hpp"

#include <SDL2/SDL.h>
#include <imgui.h>

#include <array>
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
        Keymapper& keymapper,
        WasapiAudioPlayer& audio_player
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

    struct DockItem {
        std::string package_name;
        std::string label;
        std::string monogram;
        bool is_home = false;
        bool is_active = false;
        bool is_running = false;
    };

    void UpdateAnimations(float dt, bool has_video, float win_w, float win_h, const ImGuiIO& io);
    void RenderTopSystemBar(SDL_Window* window, const TelemetrySnapshot& tel, bool& quit_requested);
    void RenderBottomDock(float win_w, float win_h, const TelemetrySnapshot& tel);
    void RenderActivitiesOverview(float win_w, float win_h, const TelemetrySnapshot& tel);
    void RenderQuickSettingsPopover(float win_w, const TelemetrySnapshot& tel);
    void RenderKeymapOverlay(const VideoRect& vrect);
    void LaunchAndroidApp(const std::string& pkg, const TelemetrySnapshot& tel);

    AppConfig& cfg_;
    SharedTelemetry& telemetry_;
    AdbManager& adb_;
    UsbTransport& transport_;
    Keymapper& keymapper_;
    WasapiAudioPlayer& audio_player_;

    ImFont* font_ui_ = nullptr;
    ImFont* font_mono_ = nullptr;
    ImFont* font_title_ = nullptr;

    std::vector<std::string> profiles_;
    char launch_pkg_buf_[128] = "com.tencent.ig";
    char overview_search_buf_[128] = "";
    bool focus_overview_search_ = false;
    bool prev_overview_open_ = false;

    // 60/120fps non-blocking UI animation states [0..1]
    float connect_anim_ = 0.0f;
    float dock_anim_ = 1.0f;
    float overview_anim_ = 0.0f;
    float popover_anim_ = 0.0f;
    std::array<float, 16> dock_hover_anim_{};
    float dock_rect_x_ = 0.0f;
    float dock_rect_y_ = 0.0f;
    float dock_rect_w_ = 0.0f;
    float dock_rect_h_ = 0.0f;

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
