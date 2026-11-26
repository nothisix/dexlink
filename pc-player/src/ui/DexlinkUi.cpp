#include "ui/DexlinkUi.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace dexlink {

namespace {

// Strict Matte Monochrome Hardware Palette (Section 1)
constexpr ImU32 kColBg = IM_COL32(0x0D, 0x0D, 0x0D, 0xFF);
constexpr ImU32 kColSurface = IM_COL32(0x16, 0x16, 0x16, 0xFF);
constexpr ImU32 kColBorder = IM_COL32(0x2A, 0x2A, 0x2A, 0xFF);
constexpr ImU32 kColText = IM_COL32(0xF0, 0xF0, 0xF0, 0xFF);
constexpr ImU32 kColMuted = IM_COL32(0x7A, 0x7A, 0x7A, 0xFF);
constexpr ImU32 kColAccent = IM_COL32(0xE5, 0xE5, 0xE5, 0xFF);
constexpr ImU32 kColConnected = IM_COL32(0x00, 0xE6, 0x76, 0xFF);
constexpr ImU32 kColError = IM_COL32(0xFF, 0x3B, 0x30, 0xFF);

ImVec4 U32ToVec4(ImU32 c) {
    return ImGui::ColorConvertU32ToFloat4(c);
}

}  // namespace

DexlinkUi::DexlinkUi(
    AppConfig& cfg,
    SharedTelemetry& telemetry,
    AdbManager& adb,
    UsbTransport& transport,
    Keymapper& keymapper
)
    : cfg_(cfg),
      telemetry_(telemetry),
      adb_(adb),
      transport_(transport),
      keymapper_(keymapper) {
    profiles_ = keymapper_.ListProfiles();
}

void DexlinkUi::InitFontsAndStyle() {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    if (fs::exists("C:\\Windows\\Fonts\\segoeui.ttf")) {
        font_ui_ = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 14.0f);
    }
    if (!font_ui_) {
        font_ui_ = io.Fonts->AddFontDefault();
    }

    if (fs::exists("C:\\Windows\\Fonts\\consola.ttf")) {
        font_mono_ = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 13.0f);
    } else {
        font_mono_ = font_ui_;
    }

    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.0f;
    st.ChildRounding = 2.0f;
    st.FrameRounding = 2.0f;
    st.PopupRounding = 2.0f;
    st.ScrollbarRounding = 2.0f;
    st.GrabRounding = 2.0f;
    st.WindowBorderSize = 1.0f;
    st.FrameBorderSize = 1.0f;
    st.PopupBorderSize = 1.0f;
    st.WindowPadding = ImVec2(8.0f, 6.0f);
    st.FramePadding = ImVec2(6.0f, 3.0f);
    st.ItemSpacing = ImVec2(6.0f, 6.0f);

    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = U32ToVec4(kColBg);
    c[ImGuiCol_ChildBg] = U32ToVec4(kColSurface);
    c[ImGuiCol_PopupBg] = U32ToVec4(kColSurface);
    c[ImGuiCol_Border] = U32ToVec4(kColBorder);
    c[ImGuiCol_Text] = U32ToVec4(kColText);
    c[ImGuiCol_TextDisabled] = U32ToVec4(kColMuted);
    c[ImGuiCol_FrameBg] = U32ToVec4(kColSurface);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.14f, 0.14f, 0.14f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
    c[ImGuiCol_Button] = U32ToVec4(kColSurface);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.16f, 0.16f, 0.16f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.22f, 0.22f, 0.22f, 1.0f);
    c[ImGuiCol_SliderGrab] = U32ToVec4(kColAccent);
    c[ImGuiCol_SliderGrabActive] = U32ToVec4(kColText);
    c[ImGuiCol_Header] = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.22f, 0.22f, 1.0f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.26f, 0.26f, 0.26f, 1.0f);
    c[ImGuiCol_Separator] = U32ToVec4(kColBorder);
}

void DexlinkUi::Render(
    SDL_Window* window,
    D3D11Renderer& renderer,
    VideoRect& out_video_rect,
    bool& out_quit_requested
) {
    const TelemetrySnapshot tel = telemetry_.Read();
    ImGuiIO& io = ImGui::GetIO();
    const float win_w = io.DisplaySize.x;
    const float win_h = io.DisplaySize.y;

    bool show_top_bar = !cfg_.fullscreen || (io.MousePos.y >= 0.0f && io.MousePos.y <= 32.0f) || cfg_.drawer_open || cfg_.keymap_edit;
    const float top_bar_h = show_top_bar ? 28.0f : 0.0f;
    const float drawer_w = cfg_.drawer_open ? 220.0f : 0.0f;

    // Video Canvas Area
    const float canvas_x = 0.0f;
    const float canvas_y = top_bar_h;
    const float canvas_w = std::max(win_w - drawer_w, 1.0f);
    const float canvas_h = std::max(win_h - top_bar_h, 1.0f);

    const int vw = std::max(renderer.VideoWidth(), cfg_.width);
    const int vh = std::max(renderer.VideoHeight(), cfg_.height);
    const float src_aspect = static_cast<float>(vw) / static_cast<float>(vh);
    const float dst_aspect = canvas_w / canvas_h;

    float draw_w = canvas_w;
    float draw_h = canvas_h;
    if (dst_aspect > src_aspect) {
        draw_w = canvas_h * src_aspect;
    } else {
        draw_h = canvas_w / src_aspect;
    }
    const float draw_x = canvas_x + (canvas_w - draw_w) * 0.5f;
    const float draw_y = canvas_y + (canvas_h - draw_h) * 0.5f;
    out_video_rect = VideoRect{draw_x, draw_y, draw_w, draw_h};

    // Draw background + live D3D11VA video texture via background draw list (zero window chrome overhead)
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), ImVec2(win_w, win_h), kColBg);
    if (renderer.VideoSrv() != nullptr) {
        bg->AddImage(
            reinterpret_cast<ImTextureID>(renderer.VideoSrv()),
            ImVec2(draw_x, draw_y),
            ImVec2(draw_x + draw_w, draw_y + draw_h)
        );
    } else {
        // Minimal monochrome standby frame when no video stream yet
        bg->AddRect(
            ImVec2(draw_x + 1.0f, draw_y + 1.0f),
            ImVec2(draw_x + draw_w - 1.0f, draw_y + draw_h - 1.0f),
            kColBorder
        );
        char wait_buf[220];
        if (!tel.last_error.empty()) {
            std::snprintf(wait_buf, sizeof(wait_buf), "USB: %s  |  %s  |  %s", tel.device_state.c_str(), tel.device_serial.c_str(), tel.last_error.c_str());
        } else if (tel.device_state == "BRIDGE") {
            std::snprintf(wait_buf, sizeof(wait_buf), "USB: BRIDGE  |  %s  |  WAIT VIDEO", tel.device_serial.c_str());
        } else {
            std::snprintf(wait_buf, sizeof(wait_buf), "USB: %s  |  %s", tel.device_state.c_str(), tel.device_serial.c_str());
        }
        bg->AddText(font_mono_, 13.0f, ImVec2(draw_x + 14.0f, draw_y + 14.0f), tel.last_error.empty() ? kColMuted : kColError, wait_buf);
    }

    if (show_top_bar) {
        RenderTopBar(window, tel, out_quit_requested);
    }
    if (cfg_.drawer_open) {
        RenderRightDrawer(tel);
    }
    if (cfg_.keymap_edit) {
        RenderKeymapOverlay(out_video_rect);
    }
}

void DexlinkUi::RenderTopBar(SDL_Window* window, const TelemetrySnapshot& tel, bool& quit_requested) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, 28.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, U32ToVec4(kColSurface));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    if (ImGui::Begin("##TopBar", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddLine(ImVec2(0, 27), ImVec2(io.DisplaySize.x, 27), kColBorder, 1.0f);

        // Left telemetry cluster (Monospace numbers & states)
        ImGui::PushFont(font_mono_);

        ImU32 dot_col = kColMuted;
        if (tel.device_state == "ERR" || tel.device_state == "NO ADB" || !tel.last_error.empty()) {
            dot_col = kColError;
        } else if (tel.video_connected) {
            dot_col = kColConnected;
        }

        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled(ImVec2(p.x + 5.0f, p.y + 10.0f), 3.5f, dot_col);
        ImGui::Dummy(ImVec2(12.0f, 18.0f));
        ImGui::SameLine(0, 2);
        ImGui::TextColored(U32ToVec4(kColText), "USB");

        ImGui::SameLine(0, 10);
        ImGui::TextColored(U32ToVec4(kColMuted), "|");
        ImGui::SameLine(0, 10);
        ImGui::Text("%dx%d", tel.video_width, tel.video_height);

        ImGui::SameLine(0, 10);
        ImGui::TextColored(U32ToVec4(kColMuted), "|");
        ImGui::SameLine(0, 10);
        float show_fps = (tel.render_fps > 0.5f) ? tel.render_fps : tel.enc_fps;
        ImGui::Text("%.0f FPS", show_fps);

        ImGui::SameLine(0, 10);
        ImGui::TextColored(U32ToVec4(kColMuted), "|");
        ImGui::SameLine(0, 10);
        ImGui::Text("%ums", tel.rtt_ms);

        ImGui::SameLine(0, 10);
        ImGui::TextColored(U32ToVec4(kColMuted), "|");
        ImGui::SameLine(0, 10);
        float mbps = (tel.rx_mbps > 0.05f) ? tel.rx_mbps : static_cast<float>(cfg_.bitrate_mbps);
        ImGui::Text("%.1f Mbps", mbps);

        ImGui::SameLine(0, 10);
        ImGui::TextColored(U32ToVec4(kColMuted), "|");
        ImGui::SameLine(0, 10);
        ImGui::TextColored(U32ToVec4(tel.thermal_warn ? kColError : kColText), "%.0fC", tel.temp_c);

        ImGui::PopFont();

        // Right tool buttons
        ImGui::PushFont(font_ui_);
        const bool gaming_on = keymapper_.GamingMode();
        const float right_w = gaming_on ? 604.0f : 576.0f;
        const float start_x = std::max(ImGui::GetCursorPosX() + 8.0f, io.DisplaySize.x - right_w);
        ImGui::SameLine(start_x);

        auto tool_btn = [&](const char* label, bool active, float w) -> bool {
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.22f, 0.22f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, U32ToVec4(kColConnected));
            }
            bool pressed = ImGui::Button(label, ImVec2(w, 21.0f));
            if (active) ImGui::PopStyleColor(2);
            return pressed;
        };

        if (tool_btn("BACK", false, 44.0f)) {
            transport_.SendControl({.cmd = protocol::ControlCmd::NavAction, .arg_u8 = 0});
        }
        ImGui::SameLine(0, 4);
        if (tool_btn("HOME", false, 46.0f)) {
            transport_.SendControl({.cmd = protocol::ControlCmd::NavAction, .arg_u8 = 1});
        }
        ImGui::SameLine(0, 4);
        if (tool_btn("CLOSE", false, 50.0f)) {
            transport_.SendControl({.cmd = protocol::ControlCmd::NavAction, .arg_u8 = 3});
        }
        ImGui::SameLine(0, 8);

        const bool is_dex_mode = (cfg_.mode == protocol::DisplayMode::Dex);
        if (tool_btn(is_dex_mode ? "DEX" : "MIRROR", is_dex_mode, 58.0f)) {
            cfg_.mode = is_dex_mode ? protocol::DisplayMode::Mirror : protocol::DisplayMode::Dex;
            adb_.UpdateConfig(cfg_);
            transport_.SendControl({
                .cmd = protocol::ControlCmd::SetMode,
                .arg_u8 = static_cast<uint8_t>(cfg_.mode),
                .arg_u16 = static_cast<uint16_t>(cfg_.dpi),
                .arg0 = 0,
                .arg1 = 0,
            });
        }
        ImGui::SameLine(0, 4);
        if (tool_btn(gaming_on ? "GAME [TAB]" : "GAME", gaming_on, gaming_on ? 74.0f : 46.0f)) {
            keymapper_.SetGamingMode(!gaming_on);
        }
        ImGui::SameLine(0, 4);
        if (tool_btn("KEYMAP", cfg_.keymap_edit, 56.0f)) {
            cfg_.keymap_edit = !cfg_.keymap_edit;
        }
        ImGui::SameLine(0, 4);
        if (tool_btn("SCREEN OFF", cfg_.screen_off, 76.0f)) {
            cfg_.screen_off = !cfg_.screen_off;
            transport_.SendControl({
                .cmd = protocol::ControlCmd::ScreenOff,
                .arg_u8 = static_cast<uint8_t>(cfg_.screen_off ? 1 : 0),
                .arg_u16 = 0,
                .arg0 = 0,
                .arg1 = 0,
            });
        }
        ImGui::SameLine(0, 4);
        if (tool_btn("SETTINGS", cfg_.drawer_open, 64.0f)) {
            cfg_.drawer_open = !cfg_.drawer_open;
        }
        ImGui::SameLine(0, 4);
        if (tool_btn("FULL", cfg_.fullscreen, 40.0f)) {
            cfg_.fullscreen = !cfg_.fullscreen;
            SDL_SetWindowFullscreen(window, cfg_.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        }
        ImGui::SameLine(0, 6);
        if (ImGui::Button("_", ImVec2(22.0f, 21.0f))) {
            SDL_MinimizeWindow(window);
        }
        ImGui::SameLine(0, 2);
        if (ImGui::Button("X", ImVec2(22.0f, 21.0f))) {
            quit_requested = true;
        }

        // Allow dragging the frameless window by holding left mouse on empty top bar space
        if (!cfg_.fullscreen && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered()) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                dragging_window_ = true;
                SDL_GetWindowPosition(window, &drag_start_wx_, &drag_start_wy_);
                SDL_GetGlobalMouseState(&drag_start_mx_, &drag_start_my_);
            }
        }
        if (dragging_window_) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                int gx = 0, gy = 0;
                SDL_GetGlobalMouseState(&gx, &gy);
                SDL_SetWindowPosition(window, drag_start_wx_ + (gx - drag_start_mx_), drag_start_wy_ + (gy - drag_start_my_));
            } else {
                dragging_window_ = false;
            }
        }
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void DexlinkUi::RenderRightDrawer(const TelemetrySnapshot& tel) {
    ImGuiIO& io = ImGui::GetIO();
    const float w = 220.0f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - w, 28.0f));
    ImGui::SetNextWindowSize(ImVec2(w, std::max(io.DisplaySize.y - 28.0f, 1.0f)));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, U32ToVec4(kColSurface));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("##RightDrawer", nullptr, flags)) {
        ImGui::PushItemWidth(-1.0f);

        auto send_res = [&]() {
            adb_.UpdateConfig(cfg_);
            transport_.SendControl({
                .cmd = protocol::ControlCmd::SetRes,
                .arg_u8 = static_cast<uint8_t>(cfg_.codec),
                .arg_u16 = static_cast<uint16_t>(cfg_.fps),
                .arg0 = static_cast<uint32_t>(cfg_.width),
                .arg1 = static_cast<uint32_t>(cfg_.height),
            });
        };

        // RES dropdown
        ImGui::TextColored(U32ToVec4(kColMuted), "RES");
        static const int res_w[] = {1920, 1600, 1280, 2560};
        static const int res_h[] = {1080, 900, 720, 1440};
        static const char* res_labels[] = {"1920x1080", "1600x900", "1280x720", "2560x1440"};
        int res_idx = 0;
        for (int i = 0; i < 4; ++i) {
            if (cfg_.width == res_w[i] && cfg_.height == res_h[i]) res_idx = i;
        }
        ImGui::PushFont(font_mono_);
        if (ImGui::Combo("##RES", &res_idx, res_labels, 4)) {
            cfg_.width = res_w[res_idx];
            cfg_.height = res_h[res_idx];
            send_res();
        }
        ImGui::PopFont();

        // FPS dropdown
        ImGui::TextColored(U32ToVec4(kColMuted), "FPS");
        static const int fps_vals[] = {60, 90, 120};
        static const char* fps_labels[] = {"60", "90", "120"};
        int fps_idx = 0;
        for (int i = 0; i < 3; ++i) {
            if (cfg_.fps == fps_vals[i]) fps_idx = i;
        }
        ImGui::PushFont(font_mono_);
        if (ImGui::Combo("##FPS", &fps_idx, fps_labels, 3)) {
            cfg_.fps = fps_vals[fps_idx];
            send_res();
        }
        ImGui::PopFont();

        // BITRATE slider (8 - 40 Mbps)
        ImGui::TextColored(U32ToVec4(kColMuted), "BITRATE");
        ImGui::PushFont(font_mono_);
        if (ImGui::SliderInt("##BITRATE", &cfg_.bitrate_mbps, 8, 40, "%d Mbps")) {
            adb_.UpdateConfig(cfg_);
            transport_.SendControl({
                .cmd = protocol::ControlCmd::SetBitrate,
                .arg_u8 = 0,
                .arg_u16 = 0,
                .arg0 = static_cast<uint32_t>(cfg_.bitrate_mbps * 1000000),
                .arg1 = 0,
            });
        }
        ImGui::PopFont();

        // MODE toggle (MIRROR / DEX)
        ImGui::TextColored(U32ToVec4(kColMuted), "DEX MODE");
        bool is_dex = (cfg_.mode == protocol::DisplayMode::Dex);
        if (ImGui::Button(is_dex ? "DEX" : "MIRROR", ImVec2(-1.0f, 24.0f))) {
            cfg_.mode = is_dex ? protocol::DisplayMode::Mirror : protocol::DisplayMode::Dex;
            adb_.UpdateConfig(cfg_);
            transport_.SendControl({
                .cmd = protocol::ControlCmd::SetMode,
                .arg_u8 = static_cast<uint8_t>(cfg_.mode),
                .arg_u16 = static_cast<uint16_t>(cfg_.dpi),
                .arg0 = 0,
                .arg1 = 0,
            });
        }

        // CODEC toggle
        ImGui::TextColored(U32ToVec4(kColMuted), "CODEC");
        bool is_hevc = (cfg_.codec == protocol::VideoCodec::Hevc);
        if (ImGui::Button(is_hevc ? "HEVC" : "H264", ImVec2(-1.0f, 24.0f))) {
            cfg_.codec = is_hevc ? protocol::VideoCodec::H264 : protocol::VideoCodec::Hevc;
            send_res();
        }

        // DPI dropdown
        ImGui::TextColored(U32ToVec4(kColMuted), "DPI");
        static const int dpi_vals[] = {240, 320, 420};
        static const char* dpi_labels[] = {"240", "320", "420"};
        int dpi_idx = 1;
        for (int i = 0; i < 3; ++i) {
            if (cfg_.dpi == dpi_vals[i]) dpi_idx = i;
        }
        ImGui::PushFont(font_mono_);
        if (ImGui::Combo("##DPI", &dpi_idx, dpi_labels, 3)) {
            cfg_.dpi = dpi_vals[dpi_idx];
            adb_.UpdateConfig(cfg_);
            transport_.SendControl({
                .cmd = protocol::ControlCmd::SetMode,
                .arg_u8 = static_cast<uint8_t>(cfg_.mode),
                .arg_u16 = static_cast<uint16_t>(cfg_.dpi),
                .arg0 = 0,
                .arg1 = 0,
            });
        }
        ImGui::PopFont();

        ImGui::Separator();

        // PROFILE selector
        ImGui::TextColored(U32ToVec4(kColMuted), "PROFILE");
        if (ImGui::BeginCombo("##PROFILE", cfg_.active_profile.c_str())) {
            profiles_ = keymapper_.ListProfiles();
            for (const auto& p : profiles_) {
                bool sel = (p == cfg_.active_profile);
                if (ImGui::Selectable(p.c_str(), sel)) {
                    cfg_.active_profile = p;
                    keymapper_.LoadProfile(p);
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("SAVE PROFILE", ImVec2(-1.0f, 24.0f))) {
            keymapper_.SaveProfile(cfg_.active_profile);
        }

        ImGui::Separator();

        // LAUNCH PKG & AUDIO APP
        ImGui::TextColored(U32ToVec4(kColMuted), "PACKAGE");
        ImGui::PushFont(font_mono_);
        ImGui::InputText("##PKG", launch_pkg_buf_, sizeof(launch_pkg_buf_));
        ImGui::PopFont();
        if (ImGui::Button("LAUNCH", ImVec2(-1.0f, 24.0f))) {
            transport_.SendControl(
                {.cmd = protocol::ControlCmd::LaunchApp},
                std::string(launch_pkg_buf_)
            );
        }

        ImGui::TextColored(U32ToVec4(kColMuted), "AUDIO");
        if (ImGui::Button(tel.audio_connected ? "PCM ACTIVE" : "OPEN APK", ImVec2(-1.0f, 24.0f))) {
            adb_.LaunchCompanionApp();
        }

        ImGui::TextColored(U32ToVec4(kColMuted), "NAV");
        if (ImGui::Button("BACK", ImVec2(62.0f, 22.0f))) {
            transport_.SendControl({.cmd = protocol::ControlCmd::NavAction, .arg_u8 = 0});
        }
        ImGui::SameLine(0, 4);
        if (ImGui::Button("HOME", ImVec2(62.0f, 22.0f))) {
            transport_.SendControl({.cmd = protocol::ControlCmd::NavAction, .arg_u8 = 1});
        }
        ImGui::SameLine(0, 4);
        if (ImGui::Button("REC", ImVec2(62.0f, 22.0f))) {
            transport_.SendControl({.cmd = protocol::ControlCmd::NavAction, .arg_u8 = 2});
        }

        ImGui::Separator();

        // Monospace Telemetry Readout
        ImGui::PushFont(font_mono_);
        ImGui::Text("BAT   %d%%", tel.battery_pct);
        ImGui::Text("DROP  %llu", static_cast<unsigned long long>(tel.dropped_frames));
        ImGui::Text("STATE %s", tel.device_state.c_str());
        ImGui::PopFont();

        ImGui::PopItemWidth();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void DexlinkUi::RenderKeymapOverlay(const VideoRect& vrect) {
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    auto& prof = keymapper_.Profile();

    auto to_screen = [&](float nx, float ny) -> ImVec2 {
        return ImVec2(vrect.x + nx * vrect.w, vrect.y + ny * vrect.h);
    };

    // WASD Joystick widget
    if (prof.joystick.enabled) {
        ImVec2 c = to_screen(prof.joystick.center_x, prof.joystick.center_y);
        float rx = prof.joystick.radius_x * vrect.w;
        float ry = prof.joystick.radius_y * vrect.h;
        fg->AddEllipse(c, ImVec2(rx, ry), kColAccent, 0.0f, 32, 1.5f);
        fg->AddRectFilled(ImVec2(c.x - 16, c.y - 9), ImVec2(c.x + 16, c.y + 9), kColSurface, 2.0f);
        fg->AddRect(ImVec2(c.x - 16, c.y - 9), ImVec2(c.x + 16, c.y + 9), kColAccent, 2.0f);
        fg->AddText(font_mono_, 12.0f, ImVec2(c.x - 13, c.y - 6), kColText, "WASD");
        // Radius handle
        fg->AddRectFilled(ImVec2(c.x + rx - 5, c.y - 5), ImVec2(c.x + rx + 5, c.y + 5), kColConnected, 1.0f);
    }

    // Mouse Look zone
    if (prof.mouse_look.enabled) {
        ImVec2 c = to_screen(prof.mouse_look.center_x, prof.mouse_look.center_y);
        float hw = prof.mouse_look.bound_half_w * vrect.w;
        float hh = prof.mouse_look.bound_half_h * vrect.h;
        fg->AddRect(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y + hh), kColBorder, 2.0f, 0, 1.5f);
        fg->AddLine(ImVec2(c.x - 8, c.y), ImVec2(c.x + 8, c.y), kColAccent, 1.5f);
        fg->AddLine(ImVec2(c.x, c.y - 8), ImVec2(c.x, c.y + 8), kColAccent, 1.5f);
        fg->AddText(font_mono_, 12.0f, ImVec2(c.x + 10, c.y - 7), kColAccent, "LOOK");
    }

    // Tap nodes
    for (int i = 0; i < static_cast<int>(prof.taps.size()); ++i) {
        const auto& t = prof.taps[i];
        ImVec2 p = to_screen(t.norm_x, t.norm_y);
        const bool sel = (i == selected_tap_idx_);
        ImU32 border = sel ? kColConnected : kColAccent;
        std::string key_str = Keymapper::InputCodeName(t.input_code);
        fg->AddRectFilled(ImVec2(p.x - 22, p.y - 12), ImVec2(p.x + 22, p.y + 12), kColSurface, 2.0f);
        fg->AddRect(ImVec2(p.x - 22, p.y - 12), ImVec2(p.x + 22, p.y + 12), border, 2.0f, 0, sel ? 2.0f : 1.0f);
        fg->AddText(font_mono_, 12.0f, ImVec2(p.x - 18, p.y - 7), kColText, key_str.c_str());
    }

    // Compact top-left toolbar inside editor
    ImGui::SetNextWindowPos(ImVec2(vrect.x + 8.0f, vrect.y + 8.0f));
    ImGui::SetNextWindowBgAlpha(0.95f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, U32ToVec4(kColSurface));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##KeymapBar", nullptr, flags)) {
        if (ImGui::Button("+ TAP", ImVec2(54, 22))) {
            prof.taps.push_back({"TAP", SDL_SCANCODE_E, 0.5f, 0.5f});
            selected_tap_idx_ = static_cast<int>(prof.taps.size()) - 1;
        }
        ImGui::SameLine();
        if (selected_tap_idx_ >= 0 && selected_tap_idx_ < static_cast<int>(prof.taps.size())) {
            if (ImGui::Button(waiting_rebind_ ? "PRESS KEY" : "REBIND", ImVec2(72, 22))) {
                waiting_rebind_ = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("DEL", ImVec2(38, 22))) {
                prof.taps.erase(prof.taps.begin() + selected_tap_idx_);
                selected_tap_idx_ = -1;
                waiting_rebind_ = false;
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("SAVE", ImVec2(48, 22))) {
            keymapper_.SaveProfile(cfg_.active_profile);
        }
        ImGui::SameLine();
        if (ImGui::Button("DONE", ImVec2(48, 22))) {
            cfg_.keymap_edit = false;
            waiting_rebind_ = false;
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

bool DexlinkUi::HandleKeymapEditorEvent(const SDL_Event& ev, const VideoRect& vrect) {
    if (!cfg_.keymap_edit) return false;
    auto& prof = keymapper_.Profile();

    if (waiting_rebind_ && selected_tap_idx_ >= 0 && selected_tap_idx_ < static_cast<int>(prof.taps.size())) {
        if (ev.type == SDL_KEYDOWN) {
            prof.taps[selected_tap_idx_].input_code = static_cast<int>(ev.key.keysym.scancode);
            waiting_rebind_ = false;
            return true;
        }
        if (ev.type == SDL_MOUSEBUTTONDOWN) {
            int code = -1;
            if (ev.button.button == SDL_BUTTON_RIGHT) code = -2;
            else if (ev.button.button == SDL_BUTTON_MIDDLE) code = -3;
            else if (ev.button.button == SDL_BUTTON_X1) code = -4;
            else if (ev.button.button == SDL_BUTTON_X2) code = -5;
            prof.taps[selected_tap_idx_].input_code = code;
            waiting_rebind_ = false;
            return true;
        }
    }

    auto dist2 = [](float ax, float ay, float bx, float by) {
        float dx = ax - bx, dy = ay - by;
        return dx * dx + dy * dy;
    };

    if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
        float mx = static_cast<float>(ev.button.x);
        float my = static_cast<float>(ev.button.y);

        // Check taps
        for (int i = 0; i < static_cast<int>(prof.taps.size()); ++i) {
            float tx = vrect.x + prof.taps[i].norm_x * vrect.w;
            float ty = vrect.y + prof.taps[i].norm_y * vrect.h;
            if (std::abs(mx - tx) <= 24.0f && std::abs(my - ty) <= 14.0f) {
                selected_tap_idx_ = i;
                dragging_node_ = DragTarget::TapNode;
                return true;
            }
        }
        // Check joystick radius handle
        float jx = vrect.x + prof.joystick.center_x * vrect.w;
        float jy = vrect.y + prof.joystick.center_y * vrect.h;
        float jrx = jx + prof.joystick.radius_x * vrect.w;
        if (dist2(mx, my, jrx, jy) <= 144.0f) {
            dragging_node_ = DragTarget::JoystickRadius;
            return true;
        }
        if (dist2(mx, my, jx, jy) <= 400.0f) {
            dragging_node_ = DragTarget::JoystickCenter;
            return true;
        }
        // Check mouse look center
        float lx = vrect.x + prof.mouse_look.center_x * vrect.w;
        float ly = vrect.y + prof.mouse_look.center_y * vrect.h;
        if (dist2(mx, my, lx, ly) <= 400.0f) {
            dragging_node_ = DragTarget::MouseLookCenter;
            return true;
        }
    } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
        if (dragging_node_ != DragTarget::None) {
            dragging_node_ = DragTarget::None;
            return true;
        }
    } else if (ev.type == SDL_MOUSEMOTION && dragging_node_ != DragTarget::None) {
        float nx = std::clamp((static_cast<float>(ev.motion.x) - vrect.x) / std::max(vrect.w, 1.0f), 0.02f, 0.98f);
        float ny = std::clamp((static_cast<float>(ev.motion.y) - vrect.y) / std::max(vrect.h, 1.0f), 0.02f, 0.98f);
        switch (dragging_node_) {
            case DragTarget::TapNode:
                if (selected_tap_idx_ >= 0 && selected_tap_idx_ < static_cast<int>(prof.taps.size())) {
                    prof.taps[selected_tap_idx_].norm_x = nx;
                    prof.taps[selected_tap_idx_].norm_y = ny;
                }
                return true;
            case DragTarget::JoystickCenter:
                prof.joystick.center_x = nx;
                prof.joystick.center_y = ny;
                return true;
            case DragTarget::JoystickRadius:
                prof.joystick.radius_x = std::clamp(nx - prof.joystick.center_x, 0.03f, 0.25f);
                prof.joystick.radius_y = prof.joystick.radius_x * (vrect.w / std::max(vrect.h, 1.0f));
                return true;
            case DragTarget::MouseLookCenter:
                prof.mouse_look.center_x = nx;
                prof.mouse_look.center_y = ny;
                return true;
            case DragTarget::None:
                break;
        }
    }

    return false;
}

}  // namespace dexlink
