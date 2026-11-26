#include "ui/DexlinkUi.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

namespace dexlink {

namespace {

// Strict Matte Monochrome Hardware Palette
constexpr ImU32 kColBg = IM_COL32(0x0D, 0x0D, 0x0D, 0xFF);
constexpr ImU32 kColSurface = IM_COL32(0x16, 0x16, 0x16, 0xFF);
constexpr ImU32 kColBorder = IM_COL32(0x2A, 0x2A, 0x2A, 0xFF);
constexpr ImU32 kColText = IM_COL32(0xF0, 0xF0, 0xF0, 0xFF);
constexpr ImU32 kColMuted = IM_COL32(0x7A, 0x7A, 0x7A, 0xFF);
constexpr ImU32 kColAccent = IM_COL32(0xE5, 0xE5, 0xE5, 0xFF);
constexpr ImU32 kColConnected = IM_COL32(0x00, 0xE6, 0x76, 0xFF);
constexpr ImU32 kColError = IM_COL32(0xFF, 0x3B, 0x30, 0xFF);

ImVec4 U32ToVec4(ImU32 c, float alpha_mul = 1.0f) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v.w *= alpha_mul;
    return v;
}

ImU32 WithAlpha(ImU32 c, float alpha_mul) {
    uint32_t a = static_cast<uint32_t>(std::clamp(((c >> IM_COL32_A_SHIFT) & 0xFF) * alpha_mul, 0.0f, 255.0f));
    return (c & ~(0xFFu << IM_COL32_A_SHIFT)) | (a << IM_COL32_A_SHIFT);
}

float EaseOutCubic(float t) {
    float inv = 1.0f - std::clamp(t, 0.0f, 1.0f);
    return 1.0f - inv * inv * inv;
}

float StepLinear(float cur, bool active, float duration_sec, float dt) {
    float step = (duration_sec > 0.001f) ? (dt / duration_sec) : 1.0f;
    if (active) return std::min(1.0f, cur + step);
    return std::max(0.0f, cur - step);
}

std::string ToLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

}  // namespace

DexlinkUi::DexlinkUi(
    AppConfig& cfg,
    SharedTelemetry& telemetry,
    AdbManager& adb,
    UsbTransport& transport,
    Keymapper& keymapper,
    WasapiAudioPlayer& audio_player
)
    : cfg_(cfg),
      telemetry_(telemetry),
      adb_(adb),
      transport_(transport),
      keymapper_(keymapper),
      audio_player_(audio_player) {
    profiles_ = keymapper_.ListProfiles();
}

void DexlinkUi::InitFontsAndStyle() {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    if (fs::exists("C:\\Windows\\Fonts\\segoeui.ttf")) {
        font_ui_ = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 14.0f);
        font_title_ = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f);
    }
    if (!font_ui_) {
        font_ui_ = io.Fonts->AddFontDefault();
    }
    if (!font_title_) {
        font_title_ = font_ui_;
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

void DexlinkUi::LaunchAndroidApp(const std::string& pkg, const TelemetrySnapshot& tel) {
    if (pkg.empty()) return;
    if (pkg == "com.dexlink.app") {
        transport_.SendControl({.cmd = protocol::ControlCmd::NavAction, .arg_u8 = 1});
        adb_.MarkAppRunning(pkg);
        return;
    }
    adb_.MarkAppRunning(pkg);
    if (tel.video_connected) {
        transport_.SendControl({.cmd = protocol::ControlCmd::LaunchApp}, pkg);
    } else {
        adb_.LaunchPackage(pkg);
    }
}

void DexlinkUi::UpdateAnimations(float dt, bool has_video, float win_w, float win_h, const ImGuiIO& io) {
    // 1. Desktop Connect Reveal (200ms)
    connect_anim_ = StepLinear(connect_anim_, has_video, 0.20f, dt);

    // 2. App Drawer / Overview Transition (180ms)
    if (cfg_.overview_open && !prev_overview_open_) {
        focus_overview_search_ = true;
        adb_.RefreshAppsAsync();
    }
    prev_overview_open_ = cfg_.overview_open;
    overview_anim_ = StepLinear(overview_anim_, cfg_.overview_open, 0.18f, dt);

    // 3. Quick Settings Popover (150ms)
    popover_anim_ = StepLinear(popover_anim_, cfg_.drawer_open, 0.15f, dt);

    // 4. Bottom Dock Auto-Hide (180ms)
    const bool auto_hide = keymapper_.GamingMode() || cfg_.fullscreen;
    const bool near_bottom = !keymapper_.GamingMode() && ImGui::IsMousePosValid(&io.MousePos) &&
                             io.MousePos.y >= (win_h - 14.0f);
    const bool inside_dock = !keymapper_.GamingMode() && ImGui::IsMousePosValid(&io.MousePos) &&
                             dock_rect_w_ > 1.0f &&
                             io.MousePos.x >= dock_rect_x_ - 6.0f &&
                             io.MousePos.x <= dock_rect_x_ + dock_rect_w_ + 6.0f &&
                             io.MousePos.y >= dock_rect_y_ - 8.0f &&
                             io.MousePos.y <= win_h;
    const bool show_dock = !auto_hide || near_bottom || (dock_anim_ > 0.1f && inside_dock) || cfg_.overview_open;
    dock_anim_ = StepLinear(dock_anim_, show_dock && (win_w > 320.0f), 0.18f, dt);
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
    const float dt = std::clamp(io.DeltaTime, 0.0001f, 0.05f);
    const bool has_video = (renderer.VideoSrv() != nullptr);

    UpdateAnimations(dt, has_video, win_w, win_h, io);

    const float connect_t = EaseOutCubic(connect_anim_);
    const float overview_t = EaseOutCubic(overview_anim_);
    const float dock_t = EaseOutCubic(dock_anim_);
    const float popover_t = EaseOutCubic(popover_anim_);

    const bool show_top_bar = !cfg_.fullscreen || (io.MousePos.y >= 0.0f && io.MousePos.y <= 34.0f) ||
                              cfg_.overview_open || cfg_.drawer_open || cfg_.keymap_edit;
    const float top_bar_h = show_top_bar ? 30.0f : 0.0f;

    // Fit Android video inside canvas below 30px top bar
    const float canvas_x = 0.0f;
    const float canvas_y = top_bar_h;
    const float canvas_w = std::max(win_w, 1.0f);
    const float canvas_h = std::max(win_h - top_bar_h, 1.0f);

    const int vw = std::max(renderer.VideoWidth(), cfg_.width);
    const int vh = std::max(renderer.VideoHeight(), cfg_.height);
    const float src_aspect = static_cast<float>(vw) / static_cast<float>(vh);
    const float dst_aspect = canvas_w / canvas_h;

    float base_w = canvas_w;
    float base_h = canvas_h;
    if (dst_aspect > src_aspect) {
        base_w = canvas_h * src_aspect;
    } else {
        base_h = canvas_w / src_aspect;
    }
    const float base_x = canvas_x + (canvas_w - base_w) * 0.5f;
    const float base_y = canvas_y + (canvas_h - base_h) * 0.5f;
    out_video_rect = VideoRect{base_x, base_y, base_w, base_h};

    // Animated viewport scale & opacity (Connect Reveal 0.97->1.0 + Overview 1.0->0.94)
    const float connect_scale = has_video ? (0.97f + 0.03f * connect_t) : 1.0f;
    const float overview_scale = 1.0f - 0.06f * overview_t;
    const float total_scale = connect_scale * overview_scale;
    const float draw_w = base_w * total_scale;
    const float draw_h = base_h * total_scale;
    const float draw_x = base_x + (base_w - draw_w) * 0.5f;
    const float draw_y = base_y + (base_h - draw_h) * 0.5f;

    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), ImVec2(win_w, win_h), kColBg);

    if (has_video) {
        const ImU32 tint = WithAlpha(IM_COL32(0xFF, 0xFF, 0xFF, 0xFF), connect_t);
        bg->AddImage(
            reinterpret_cast<ImTextureID>(renderer.VideoSrv()),
            ImVec2(draw_x, draw_y),
            ImVec2(draw_x + draw_w, draw_y + draw_h),
            ImVec2(0, 0),
            ImVec2(1, 1),
            tint
        );
        if (overview_t > 0.005f) {
            bg->AddRectFilled(
                ImVec2(canvas_x, canvas_y),
                ImVec2(win_w, win_h),
                WithAlpha(kColBg, 0.72f * overview_t)
            );
            bg->AddRect(
                ImVec2(draw_x, draw_y),
                ImVec2(draw_x + draw_w, draw_y + draw_h),
                WithAlpha(kColBorder, overview_t),
                2.0f
            );
        }
    } else {
        bg->AddRect(
            ImVec2(base_x + 1.0f, base_y + 1.0f),
            ImVec2(base_x + base_w - 1.0f, base_y + base_h - 1.0f),
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
        bg->AddText(font_mono_, 13.0f, ImVec2(base_x + 14.0f, base_y + 14.0f), tel.last_error.empty() ? kColMuted : kColError, wait_buf);
    }

    if (show_top_bar) {
        RenderTopSystemBar(window, tel, out_quit_requested);
    }
    if (overview_t > 0.01f) {
        RenderActivitiesOverview(win_w, win_h, tel);
    }
    if (dock_t > 0.01f) {
        RenderBottomDock(win_w, win_h, tel);
    }
    if (popover_t > 0.01f) {
        RenderQuickSettingsPopover(win_w, tel);
    }
    if (cfg_.keymap_edit) {
        RenderKeymapOverlay(out_video_rect);
    }
}

void DexlinkUi::RenderTopSystemBar(SDL_Window* window, const TelemetrySnapshot& tel, bool& quit_requested) {
    ImGuiIO& io = ImGui::GetIO();
    const float win_w = io.DisplaySize.x;
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(win_w, 30.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, U32ToVec4(kColBg));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    if (ImGui::Begin("##TopSystemBar", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddLine(ImVec2(0.0f, 29.0f), ImVec2(win_w, 29.0f), kColBorder, 1.0f);

        auto top_btn = [&](const char* label, bool active, float w, ImU32 active_col = kColConnected) -> bool {
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Border, U32ToVec4(active_col));
                ImGui::PushStyleColor(ImGuiCol_Text, U32ToVec4(active_col));
            }
            bool pressed = ImGui::Button(label, ImVec2(w, 21.0f));
            if (active) ImGui::PopStyleColor(3);
            return pressed;
        };

        // LEFT: APPS / OVERVIEW trigger + quick Android BACK / HOME / CLOSE
        ImGui::PushFont(font_ui_);
        if (top_btn("APPS", cfg_.overview_open, 52.0f)) {
            cfg_.overview_open = !cfg_.overview_open;
            if (cfg_.overview_open && keymapper_.GamingMode()) keymapper_.SetGamingMode(false);
        }
        ImGui::SameLine(0, 4);
        if (top_btn("BACK", false, 42.0f)) {
            transport_.SendControl({.cmd = protocol::ControlCmd::NavAction, .arg_u8 = 0});
        }
        ImGui::SameLine(0, 4);
        if (top_btn("HOME", false, 44.0f)) {
            transport_.SendControl({.cmd = protocol::ControlCmd::NavAction, .arg_u8 = 1});
            adb_.MarkAppRunning("com.dexlink.app");
        }
        ImGui::SameLine(0, 4);
        if (top_btn("CLOSE", false, 48.0f)) {
            transport_.SendControl({.cmd = protocol::ControlCmd::NavAction, .arg_u8 = 3});
            adb_.MarkAppRunning("com.dexlink.app");
        }
        ImGui::PopFont();

        // CENTER: Monospace Clock HH:MM + compact live telemetry pill (1080p 60 FPS 11ms)
        std::time_t now_t = std::time(nullptr);
        std::tm local_tm{};
        localtime_s(&local_tm, &now_t);
        float show_fps = (tel.render_fps > 0.5f) ? tel.render_fps : tel.enc_fps;
        int show_h = (tel.video_height > 0) ? tel.video_height : cfg_.height;
        char center_buf[80];
        std::snprintf(
            center_buf,
            sizeof(center_buf),
            "%02d:%02d   %dp  %.0f FPS  %ums",
            local_tm.tm_hour,
            local_tm.tm_min,
            show_h,
            show_fps,
            tel.rtt_ms
        );
        ImGui::PushFont(font_mono_);
        ImVec2 txt_sz = ImGui::CalcTextSize(center_buf);
        const float badge_w = txt_sz.x + 18.0f;
        const float badge_x = std::max(216.0f, (win_w - badge_w) * 0.5f);
        if (badge_x + badge_w < win_w - 375.0f) {
            dl->AddRectFilled(ImVec2(badge_x, 4.0f), ImVec2(badge_x + badge_w, 25.0f), kColSurface, 2.0f);
            dl->AddRect(ImVec2(badge_x, 4.0f), ImVec2(badge_x + badge_w, 25.0f), kColBorder, 2.0f);
            dl->AddText(font_mono_, 13.0f, ImVec2(badge_x + 9.0f, 8.0f), kColText, center_buf);
        }
        ImGui::PopFont();

        // RIGHT: System Tray (USB Dot + Battery % + Temp C + Mute + Screen-Off + Keymap + Quick Settings + Win)
        const float right_w = 398.0f;
        const float start_x = std::max(ImGui::GetCursorPosX() + 8.0f, win_w - right_w);
        ImGui::SameLine(start_x);

        ImGui::PushFont(font_mono_);
        ImU32 dot_col = kColMuted;
        if (tel.device_state == "ERR" || tel.device_state == "NO ADB" || !tel.last_error.empty()) {
            dot_col = kColError;
        } else if (tel.video_connected) {
            dot_col = kColConnected;
        }
        const ImVec2 dp = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled(ImVec2(dp.x + 4.0f, dp.y + 10.0f), 3.5f, dot_col);
        ImGui::Dummy(ImVec2(10.0f, 18.0f));
        ImGui::SameLine(0, 2);
        ImGui::TextColored(U32ToVec4(kColText), "%d%%", tel.battery_pct);
        ImGui::SameLine(0, 6);
        ImGui::TextColored(U32ToVec4(tel.thermal_warn ? kColError : kColMuted), "%.0fC", tel.temp_c);
        ImGui::PopFont();

        ImGui::PushFont(font_ui_);
        ImGui::SameLine(0, 8);
        if (top_btn(cfg_.audio_muted ? "MUTE" : "AUD", cfg_.audio_muted, 42.0f, kColError)) {
            cfg_.audio_muted = !cfg_.audio_muted;
            audio_player_.SetMuted(cfg_.audio_muted);
        }
        ImGui::SameLine(0, 4);
        if (top_btn("SCR OFF", cfg_.screen_off, 58.0f)) {
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
        if (top_btn("KEYMAP", cfg_.keymap_edit, 56.0f)) {
            cfg_.keymap_edit = !cfg_.keymap_edit;
            if (cfg_.keymap_edit && keymapper_.GamingMode()) keymapper_.SetGamingMode(false);
        }
        ImGui::SameLine(0, 4);
        if (top_btn("TRAY", cfg_.drawer_open, 42.0f)) {
            cfg_.drawer_open = !cfg_.drawer_open;
        }
        ImGui::SameLine(0, 6);
        if (ImGui::Button("_", ImVec2(22.0f, 21.0f))) {
            SDL_MinimizeWindow(window);
        }
        ImGui::SameLine(0, 2);
        if (ImGui::Button("X", ImVec2(22.0f, 21.0f))) {
            quit_requested = true;
        }
        ImGui::PopFont();

        // Frameless window drag on empty top bar background
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
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void DexlinkUi::RenderBottomDock(float win_w, float win_h, const TelemetrySnapshot& tel) {
    const float alpha = EaseOutCubic(dock_anim_);
    const float translate_y = (1.0f - alpha) * 56.0f;

    const auto apps = adb_.GetInstalledApps();
    const std::string active_pkg = adb_.ActivePackage();

    std::vector<DockItem> items;
    auto add_pinned = [&](const std::string& pkg, const std::string& label, const std::string& mono, bool home) {
        bool running = (pkg == active_pkg);
        for (const auto& a : apps) {
            if (a.package_name == pkg && a.is_running) running = true;
        }
        items.push_back({pkg, label, mono, home, pkg == active_pkg, running});
    };

    add_pinned("com.dexlink.app", "DEX DESKTOP", "DX", true);
    add_pinned("com.android.settings", "SETTINGS", "ST", false);
    add_pinned("com.android.chrome", "CHROME", "CH", false);
    add_pinned("com.android.vending", "PLAY", "PS", false);

    // Add pinned games / active running apps if installed or active
    for (const auto& a : apps) {
        if (items.size() >= 9) break;
        bool already = false;
        for (const auto& existing : items) {
            if (existing.package_name == a.package_name) { already = true; break; }
        }
        if (already) continue;
        const bool pinned_game = (a.package_name == "com.tencent.ig" ||
                                  a.package_name == "com.miHoYo.GenshinImpact" ||
                                  a.package_name == "com.mobilelegends.mi" ||
                                  a.package_name == "com.discord");
        if (a.is_running || pinned_game) {
            items.push_back({a.package_name, a.short_name, a.monogram, false, a.package_name == active_pkg, a.is_running});
        }
    }
    if (!active_pkg.empty()) {
        bool found = false;
        for (const auto& existing : items) {
            if (existing.package_name == active_pkg) { found = true; break; }
        }
        if (!found && items.size() < 10) {
            items.push_back({active_pkg, active_pkg, "AP", false, true, true});
        }
    }

    const float icon_sz = 30.0f;
    const float icon_gap = 6.0f;
    const float pad_x = 10.0f;
    const float icons_w = items.size() * icon_sz + (items.empty() ? 0.0f : (items.size() - 1) * icon_gap);
    const float game_w = keymapper_.GamingMode() ? 72.0f : 48.0f;
    const float actions_w = game_w + 4.0f + 56.0f + 4.0f + 46.0f; // GAME + KEYMAP + GRID
    const float dock_w = pad_x * 2.0f + icons_w + 15.0f + actions_w;
    const float dock_h = 44.0f;
    const float dock_x = std::max(8.0f, (win_w - dock_w) * 0.5f);
    const float dock_y = win_h - 8.0f - dock_h + translate_y;

    dock_rect_x_ = dock_x;
    dock_rect_y_ = dock_y;
    dock_rect_w_ = dock_w;
    dock_rect_h_ = dock_h;

    ImGui::SetNextWindowPos(ImVec2(dock_x, dock_y));
    ImGui::SetNextWindowSize(ImVec2(dock_w, dock_h));

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad_x, 6.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, U32ToVec4(kColSurface));
    ImGui::PushStyleColor(ImGuiCol_Border, U32ToVec4(kColBorder));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    if (ImGui::Begin("##FloatingDock", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0001f, 0.05f);
        const ImVec2 mouse = ImGui::GetIO().MousePos;

        for (size_t i = 0; i < items.size(); ++i) {
            if (i > 0) ImGui::SameLine(0, icon_gap);
            const auto& it = items[i];
            const ImVec2 base_pos = ImGui::GetCursorScreenPos();

            ImGui::PushID(static_cast<int>(i));
            bool clicked = ImGui::InvisibleButton("##dockicon", ImVec2(icon_sz, icon_sz));
            bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();

            const size_t anim_idx = std::min(i, dock_hover_anim_.size() - 1);
            dock_hover_anim_[anim_idx] = StepLinear(dock_hover_anim_[anim_idx], hovered, 0.10f, dt);
            const float lift_y = -2.0f * EaseOutCubic(dock_hover_anim_[anim_idx]);

            const ImVec2 box_min(base_pos.x, base_pos.y + lift_y);
            const ImVec2 box_max(base_pos.x + icon_sz, base_pos.y + icon_sz + lift_y);
            const ImU32 box_bg = hovered ? IM_COL32(0x22, 0x22, 0x22, 0xFF) : kColBg;
            const ImU32 box_border = it.is_active ? kColConnected : (hovered ? kColAccent : kColBorder);

            dl->AddRectFilled(box_min, box_max, WithAlpha(box_bg, alpha), 2.0f);
            dl->AddRect(box_min, box_max, WithAlpha(box_border, alpha), 2.0f);

            ImGui::PushFont(font_mono_);
            ImVec2 msz = ImGui::CalcTextSize(it.monogram.c_str());
            dl->AddText(
                font_mono_,
                13.0f,
                ImVec2(box_min.x + (icon_sz - msz.x) * 0.5f, box_min.y + (icon_sz - msz.y) * 0.5f),
                WithAlpha(it.is_active ? kColConnected : kColText, alpha),
                it.monogram.c_str()
            );
            ImGui::PopFont();

            // Crisp 3px active/running indicator bar underneath icon
            if (it.is_active || it.is_running) {
                const float bar_w = it.is_active ? 16.0f : 8.0f;
                const float bar_x = base_pos.x + (icon_sz - bar_w) * 0.5f;
                const float bar_y = dock_y + dock_h - 5.0f;
                dl->AddRectFilled(
                    ImVec2(bar_x, bar_y),
                    ImVec2(bar_x + bar_w, bar_y + 3.0f),
                    WithAlpha(it.is_active ? kColConnected : kColAccent, alpha),
                    0.0f
                );
            }

            if (hovered && ImGui::IsMousePosValid(&mouse)) {
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                ImVec2 lsz = ImGui::CalcTextSize(it.label.c_str());
                const float tw = lsz.x + 12.0f;
                const float tx = base_pos.x + (icon_sz - tw) * 0.5f;
                const float ty = dock_y - 26.0f;
                fg->AddRectFilled(ImVec2(tx, ty), ImVec2(tx + tw, ty + 20.0f), kColBg, 2.0f);
                fg->AddRect(ImVec2(tx, ty), ImVec2(tx + tw, ty + 20.0f), kColBorder, 2.0f);
                fg->AddText(font_mono_, 12.0f, ImVec2(tx + 6.0f, ty + 3.0f), kColText, it.label.c_str());
            }

            if (clicked) {
                cfg_.overview_open = false;
                LaunchAndroidApp(it.package_name, tel);
            }
        }

        // 1px Vertical Divider
        ImGui::SameLine(0, 7);
        const ImVec2 div_p = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(div_p.x, dock_y + 7.0f), ImVec2(div_p.x, dock_y + dock_h - 7.0f), WithAlpha(kColBorder, alpha), 1.0f);
        ImGui::Dummy(ImVec2(1.0f, icon_sz));
        ImGui::SameLine(0, 7);

        auto dock_btn = [&](const char* lbl, bool active, float w) -> bool {
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.20f, 0.20f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Border, U32ToVec4(kColConnected));
                ImGui::PushStyleColor(ImGuiCol_Text, U32ToVec4(kColConnected));
            }
            bool pressed = ImGui::Button(lbl, ImVec2(w, icon_sz));
            if (active) ImGui::PopStyleColor(3);
            return pressed;
        };

        const bool gaming_on = keymapper_.GamingMode();
        if (dock_btn(gaming_on ? "GAME [TAB]" : "GAME", gaming_on, game_w)) {
            keymapper_.SetGamingMode(!gaming_on);
        }
        ImGui::SameLine(0, 4);
        if (dock_btn("KEYMAP", cfg_.keymap_edit, 56.0f)) {
            cfg_.keymap_edit = !cfg_.keymap_edit;
            if (cfg_.keymap_edit && gaming_on) keymapper_.SetGamingMode(false);
        }
        ImGui::SameLine(0, 4);
        if (dock_btn("GRID", cfg_.overview_open, 46.0f)) {
            cfg_.overview_open = !cfg_.overview_open;
            if (cfg_.overview_open && gaming_on) keymapper_.SetGamingMode(false);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
}

void DexlinkUi::RenderActivitiesOverview(float win_w, float win_h, const TelemetrySnapshot& tel) {
    const float alpha = EaseOutCubic(overview_anim_);
    const float panel_w = std::min(820.0f, win_w - 48.0f);
    const float panel_h = std::min(480.0f, win_h - 110.0f);
    const float panel_x = (win_w - panel_w) * 0.5f;
    const float panel_y = 30.0f + (win_h - 90.0f - panel_h) * 0.5f + (1.0f - alpha) * 10.0f;

    ImGui::SetNextWindowPos(ImVec2(panel_x, panel_y));
    ImGui::SetNextWindowSize(ImVec2(panel_w, panel_h));

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, U32ToVec4(kColBg, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, U32ToVec4(kColBorder));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("##ActivitiesOverview", nullptr, flags)) {
        // Search Bar row
        const float btn_sync_w = 64.0f;
        const float btn_close_w = 56.0f;
        const float search_w = std::max(140.0f, panel_w - 28.0f - btn_sync_w - btn_close_w - 12.0f);

        ImGui::PushFont(font_mono_);
        ImGui::SetNextItemWidth(search_w);
        if (focus_overview_search_) {
            ImGui::SetKeyboardFocusHere();
            focus_overview_search_ = false;
        }
        ImGui::InputTextWithHint("##OverviewSearch", "Search apps...", overview_search_buf_, sizeof(overview_search_buf_));
        ImGui::PopFont();

        ImGui::SameLine(0, 6);
        if (ImGui::Button("SYNC", ImVec2(btn_sync_w, 22.0f))) {
            adb_.RefreshAppsAsync();
        }
        ImGui::SameLine(0, 6);
        if (ImGui::Button("ESC", ImVec2(btn_close_w, 22.0f))) {
            cfg_.overview_open = false;
        }

        ImGui::Separator();

        const auto all_apps = adb_.GetInstalledApps();
        const std::string active_pkg = adb_.ActivePackage();
        const std::string query = ToLowerAscii(overview_search_buf_);

        std::vector<AndroidAppInfo> filtered;
        filtered.reserve(all_apps.size());
        for (const auto& app : all_apps) {
            if (!query.empty()) {
                if (ToLowerAscii(app.short_name).find(query) == std::string::npos &&
                    ToLowerAscii(app.package_name).find(query) == std::string::npos) {
                    continue;
                }
            }
            filtered.push_back(app);
        }

        if (ImGui::BeginChild("##OverviewGrid", ImVec2(0, 0), false)) {
            if (filtered.empty()) {
                ImGui::PushFont(font_mono_);
                ImGui::TextColored(
                    U32ToVec4(kColMuted),
                    tel.usb_attached ? "NO PACKAGES MATCHING QUERY" : "USB DEVICE DISCONNECTED"
                );
                ImGui::PopFont();
            } else {
                const int cols = std::max(2, static_cast<int>((panel_w - 28.0f) / 190.0f));
                const float card_w = (panel_w - 36.0f - (cols - 1) * 8.0f) / static_cast<float>(cols);
                const float card_h = 52.0f;
                ImDrawList* dl = ImGui::GetWindowDrawList();

                for (size_t i = 0; i < filtered.size(); ++i) {
                    if (i % cols != 0) ImGui::SameLine(0, 8.0f);
                    const auto& app = filtered[i];
                    const bool is_active = (app.package_name == active_pkg);
                    const ImVec2 pos = ImGui::GetCursorScreenPos();

                    ImGui::PushID(static_cast<int>(i));
                    bool clicked = ImGui::InvisibleButton("##appcard", ImVec2(card_w, card_h));
                    bool hovered = ImGui::IsItemHovered();
                    ImGui::PopID();

                    const ImU32 card_bg = hovered ? IM_COL32(0x1E, 0x1E, 0x1E, 0xFF) : kColSurface;
                    const ImU32 card_border = is_active ? kColConnected : (hovered ? kColAccent : kColBorder);
                    dl->AddRectFilled(pos, ImVec2(pos.x + card_w, pos.y + card_h), WithAlpha(card_bg, alpha), 2.0f);
                    dl->AddRect(pos, ImVec2(pos.x + card_w, pos.y + card_h), WithAlpha(card_border, alpha), 2.0f);

                    // Monogram square
                    const ImVec2 mb_min(pos.x + 10.0f, pos.y + 10.0f);
                    const ImVec2 mb_max(pos.x + 42.0f, pos.y + 42.0f);
                    dl->AddRectFilled(mb_min, mb_max, WithAlpha(kColBg, alpha), 2.0f);
                    dl->AddRect(mb_min, mb_max, WithAlpha(kColBorder, alpha), 2.0f);

                    ImGui::PushFont(font_mono_);
                    ImVec2 msz = ImGui::CalcTextSize(app.monogram.c_str());
                    dl->AddText(
                        font_mono_,
                        13.0f,
                        ImVec2(mb_min.x + (32.0f - msz.x) * 0.5f, mb_min.y + (32.0f - msz.y) * 0.5f),
                        WithAlpha(is_active ? kColConnected : kColText, alpha),
                        app.monogram.c_str()
                    );
                    ImGui::PopFont();

                    const float tx = pos.x + 50.0f;
                    const ImVec2 clip_max(pos.x + card_w - 8.0f, pos.y + card_h - 4.0f);
                    dl->PushClipRect(ImVec2(tx, pos.y), clip_max, true);
                    dl->AddText(font_ui_, 14.0f, ImVec2(tx, pos.y + 9.0f), WithAlpha(kColText, alpha), app.short_name.c_str());
                    dl->AddText(font_mono_, 11.0f, ImVec2(tx, pos.y + 29.0f), WithAlpha(kColMuted, alpha), app.package_name.c_str());
                    dl->PopClipRect();

                    if (clicked) {
                        cfg_.overview_open = false;
                        LaunchAndroidApp(app.package_name, tel);
                    }
                }
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
}

void DexlinkUi::RenderQuickSettingsPopover(float win_w, const TelemetrySnapshot& tel) {
    const float alpha = EaseOutCubic(popover_anim_);
    const float w = 240.0f;
    const float slide_y = -8.0f * (1.0f - alpha);
    const float x = std::max(8.0f, win_w - w - 8.0f);
    const float y = 36.0f + slide_y;

    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 120.0f), ImVec2(w, 620.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, U32ToVec4(kColSurface));
    ImGui::PushStyleColor(ImGuiCol_Border, U32ToVec4(kColBorder));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("##QuickSettingsPopover", nullptr, flags)) {
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

        ImGui::TextColored(U32ToVec4(kColMuted), "MODE");
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

        ImGui::TextColored(U32ToVec4(kColMuted), "CODEC");
        bool is_hevc = (cfg_.codec == protocol::VideoCodec::Hevc);
        if (ImGui::Button(is_hevc ? "HEVC" : "H264", ImVec2(106.0f, 22.0f))) {
            cfg_.codec = is_hevc ? protocol::VideoCodec::H264 : protocol::VideoCodec::Hevc;
            send_res();
        }
        ImGui::SameLine(0, 6);
        if (ImGui::Button(cfg_.screen_off ? "SCR ON" : "SCR OFF", ImVec2(108.0f, 22.0f))) {
            cfg_.screen_off = !cfg_.screen_off;
            transport_.SendControl({
                .cmd = protocol::ControlCmd::ScreenOff,
                .arg_u8 = static_cast<uint8_t>(cfg_.screen_off ? 1 : 0),
                .arg_u16 = 0,
                .arg0 = 0,
                .arg1 = 0,
            });
        }

        ImGui::Separator();

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
        if (ImGui::Button("SAVE PROFILE", ImVec2(-1.0f, 22.0f))) {
            keymapper_.SaveProfile(cfg_.active_profile);
        }

        ImGui::Separator();

        ImGui::TextColored(U32ToVec4(kColMuted), "PACKAGE");
        ImGui::PushFont(font_mono_);
        ImGui::InputText("##PKG", launch_pkg_buf_, sizeof(launch_pkg_buf_));
        ImGui::PopFont();
        if (ImGui::Button("LAUNCH", ImVec2(106.0f, 22.0f))) {
            LaunchAndroidApp(std::string(launch_pkg_buf_), tel);
        }
        ImGui::SameLine(0, 6);
        if (ImGui::Button(tel.audio_connected ? "PCM ON" : "OPEN APK", ImVec2(108.0f, 22.0f))) {
            adb_.LaunchCompanionApp();
        }

        ImGui::Separator();

        ImGui::PushFont(font_mono_);
        ImGui::Text("DROP %llu  RX %.1fM", static_cast<unsigned long long>(tel.dropped_frames), tel.rx_mbps);
        ImGui::Text("STATE %s", tel.device_state.c_str());
        ImGui::PopFont();

        ImGui::PopItemWidth();
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
}

void DexlinkUi::RenderKeymapOverlay(const VideoRect& vrect) {
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    auto& prof = keymapper_.Profile();

    auto to_screen = [&](float nx, float ny) -> ImVec2 {
        return ImVec2(vrect.x + nx * vrect.w, vrect.y + ny * vrect.h);
    };

    if (prof.joystick.enabled) {
        ImVec2 c = to_screen(prof.joystick.center_x, prof.joystick.center_y);
        float rx = prof.joystick.radius_x * vrect.w;
        float ry = prof.joystick.radius_y * vrect.h;
        fg->AddEllipse(c, ImVec2(rx, ry), kColAccent, 0.0f, 32, 1.5f);
        fg->AddRectFilled(ImVec2(c.x - 16, c.y - 9), ImVec2(c.x + 16, c.y + 9), kColSurface, 2.0f);
        fg->AddRect(ImVec2(c.x - 16, c.y - 9), ImVec2(c.x + 16, c.y + 9), kColAccent, 2.0f);
        fg->AddText(font_mono_, 12.0f, ImVec2(c.x - 13, c.y - 6), kColText, "WASD");
        fg->AddRectFilled(ImVec2(c.x + rx - 5, c.y - 5), ImVec2(c.x + rx + 5, c.y + 5), kColConnected, 1.0f);
    }

    if (prof.mouse_look.enabled) {
        ImVec2 c = to_screen(prof.mouse_look.center_x, prof.mouse_look.center_y);
        float hw = prof.mouse_look.bound_half_w * vrect.w;
        float hh = prof.mouse_look.bound_half_h * vrect.h;
        fg->AddRect(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y + hh), kColBorder, 2.0f, 0, 1.5f);
        fg->AddLine(ImVec2(c.x - 8, c.y), ImVec2(c.x + 8, c.y), kColAccent, 1.5f);
        fg->AddLine(ImVec2(c.x, c.y - 8), ImVec2(c.x, c.y + 8), kColAccent, 1.5f);
        fg->AddText(font_mono_, 12.0f, ImVec2(c.x + 10, c.y - 7), kColAccent, "LOOK");
    }

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

        for (int i = 0; i < static_cast<int>(prof.taps.size()); ++i) {
            float tx = vrect.x + prof.taps[i].norm_x * vrect.w;
            float ty = vrect.y + prof.taps[i].norm_y * vrect.h;
            if (std::abs(mx - tx) <= 24.0f && std::abs(my - ty) <= 14.0f) {
                selected_tap_idx_ = i;
                dragging_node_ = DragTarget::TapNode;
                return true;
            }
        }
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
