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

// Strict Matte Samsung DeX / Hardware Palette
constexpr ImU32 kColBg = IM_COL32(0x0D, 0x0D, 0x0D, 0xFF);
constexpr ImU32 kColTaskbar = IM_COL32(0x14, 0x14, 0x14, 0xEB); // #141414 @ ~92% opacity
constexpr ImU32 kColSurface = IM_COL32(0x16, 0x16, 0x16, 0xFF);
constexpr ImU32 kColSurfaceHover = IM_COL32(0x28, 0x28, 0x28, 0xB8);
constexpr ImU32 kColPill = IM_COL32(0x1C, 0x1C, 0x1C, 0xEA);
constexpr ImU32 kColBorder = IM_COL32(0x2A, 0x2A, 0x2A, 0xFF);
constexpr ImU32 kColText = IM_COL32(0xF0, 0xF0, 0xF0, 0xFF);
constexpr ImU32 kColMuted = IM_COL32(0x88, 0x88, 0x88, 0xFF);
constexpr ImU32 kColAccent = IM_COL32(0xE5, 0xE5, 0xE5, 0xFF);
constexpr ImU32 kColConnected = IM_COL32(0x00, 0xE6, 0x76, 0xFF);
constexpr ImU32 kColError = IM_COL32(0xFF, 0x3B, 0x30, 0xFF);

constexpr float kTaskbarHeight = 44.0f;

ImVec4 U32ToVec4(ImU32 c, float alpha_mul = 1.0f) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v.w *= alpha_mul;
    return v;
}

ImU32 WithAlpha(ImU32 c, float alpha_mul) {
    uint32_t a = static_cast<uint32_t>(
        std::clamp(((c >> IM_COL32_A_SHIFT) & 0xFF) * alpha_mul, 0.0f, 255.0f)
    );
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

std::string TruncateTextToWidth(ImFont* font, float font_size, const std::string& text, float max_w) {
    if (!font || text.empty()) return text;
    if (font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.c_str()).x <= max_w) {
        return text;
    }
    const std::string dots = "...";
    float dots_w = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, dots.c_str()).x;
    std::string cur = text;
    while (!cur.empty()) {
        cur.pop_back();
        // Trim any partial UTF-8 continuation bytes cleanly
        while (!cur.empty() && (static_cast<unsigned char>(cur.back()) & 0xC0) == 0x80) {
            cur.pop_back();
        }
        float w = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, cur.c_str()).x;
        if (w + dots_w <= max_w) {
            return cur + dots;
        }
    }
    return dots;
}

void DrawTooltipAboveTaskbar(ImFont* font, float anchor_center_x, float taskbar_y, const char* label) {
    if (!label || !label[0]) return;
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    ImVec2 sz = font->CalcTextSizeA(12.0f, FLT_MAX, 0.0f, label);
    float tw = sz.x + 14.0f;
    float th = 22.0f;
    float tx = std::clamp(anchor_center_x - tw * 0.5f, 8.0f, ImGui::GetIO().DisplaySize.x - tw - 8.0f);
    float ty = taskbar_y - th - 6.0f;
    fg->AddRectFilled(ImVec2(tx, ty), ImVec2(tx + tw, ty + th), kColSurface, 4.0f);
    fg->AddRect(ImVec2(tx, ty), ImVec2(tx + tw, ty + th), kColBorder, 4.0f);
    fg->AddText(font, 12.0f, ImVec2(tx + 7.0f, ty + 4.0f), kColText, label);
}

// Clean monochrome vector icons drawn on ImDrawList (no ASCII hacks, no emojis)
void DrawNavRecentsIcon(ImDrawList* dl, ImVec2 center, ImU32 col) {
    float x = center.x;
    float y = center.y;
    dl->AddLine(ImVec2(x - 5.0f, y - 6.0f), ImVec2(x - 5.0f, y + 6.0f), col, 1.7f);
    dl->AddLine(ImVec2(x, y - 6.0f), ImVec2(x, y + 6.0f), col, 1.7f);
    dl->AddLine(ImVec2(x + 5.0f, y - 6.0f), ImVec2(x + 5.0f, y + 6.0f), col, 1.7f);
}

void DrawNavHomeIcon(ImDrawList* dl, ImVec2 center, ImU32 col) {
    dl->AddCircle(center, 6.2f, col, 20, 1.7f);
}

void DrawNavBackIcon(ImDrawList* dl, ImVec2 center, ImU32 col) {
    dl->AddLine(ImVec2(center.x + 3.5f, center.y - 6.0f), ImVec2(center.x - 3.5f, center.y), col, 1.8f);
    dl->AddLine(ImVec2(center.x - 3.5f, center.y), ImVec2(center.x + 3.5f, center.y + 6.0f), col, 1.8f);
}

void DrawSearchIcon(ImDrawList* dl, ImVec2 center, ImU32 col) {
    ImVec2 c(center.x - 1.5f, center.y - 1.5f);
    dl->AddCircle(c, 5.2f, col, 18, 1.6f);
    dl->AddLine(ImVec2(c.x + 3.7f, c.y + 3.7f), ImVec2(c.x + 7.5f, c.y + 7.5f), col, 1.7f);
}

void DrawNineDotIcon(ImDrawList* dl, ImVec2 center, ImU32 col) {
    constexpr float step = 4.8f;
    for (int r = -1; r <= 1; ++r) {
        for (int c = -1; c <= 1; ++c) {
            dl->AddCircleFilled(ImVec2(center.x + c * step, center.y + r * step), 1.45f, col, 8);
        }
    }
}

void DrawGamepadIcon(ImDrawList* dl, ImVec2 center, ImU32 col) {
    ImVec2 p0(center.x - 7.5f, center.y - 4.5f);
    ImVec2 p1(center.x + 7.5f, center.y + 4.5f);
    dl->AddRect(p0, p1, col, 3.0f, 0, 1.5f);
    // D-pad on left
    dl->AddLine(ImVec2(center.x - 5.0f, center.y), ImVec2(center.x - 1.5f, center.y), col, 1.3f);
    dl->AddLine(ImVec2(center.x - 3.25f, center.y - 1.8f), ImVec2(center.x - 3.25f, center.y + 1.8f), col, 1.3f);
    // AB dots on right
    dl->AddCircleFilled(ImVec2(center.x + 2.8f, center.y + 1.2f), 1.1f, col);
    dl->AddCircleFilled(ImVec2(center.x + 4.8f, center.y - 1.2f), 1.1f, col);
}

void DrawKeymapIcon(ImDrawList* dl, ImVec2 center, ImU32 col) {
    dl->AddRect(ImVec2(center.x - 7.5f, center.y - 5.0f), ImVec2(center.x + 7.5f, center.y + 5.0f), col, 2.0f, 0, 1.4f);
    for (int i = -1; i <= 1; ++i) {
        dl->AddCircleFilled(ImVec2(center.x + i * 3.5f, center.y - 1.5f), 0.9f, col);
    }
    dl->AddLine(ImVec2(center.x - 4.0f, center.y + 2.0f), ImVec2(center.x + 4.0f, center.y + 2.0f), col, 1.3f);
}

void DrawScreenOffIcon(ImDrawList* dl, ImVec2 center, ImU32 col, bool active) {
    dl->AddRect(ImVec2(center.x - 4.5f, center.y - 6.8f), ImVec2(center.x + 4.5f, center.y + 6.8f), col, 1.5f, 0, 1.4f);
    if (active) {
        dl->AddLine(ImVec2(center.x - 6.5f, center.y - 5.5f), ImVec2(center.x + 6.5f, center.y + 5.5f), col, 1.5f);
    } else {
        dl->AddLine(ImVec2(center.x - 1.8f, center.y + 4.2f), ImVec2(center.x + 1.8f, center.y + 4.2f), col, 1.4f);
    }
}

void DrawAudioIcon(ImDrawList* dl, ImVec2 center, ImU32 col, bool muted) {
    // Speaker cone
    ImVec2 pts[4] = {
        ImVec2(center.x - 6.5f, center.y - 2.3f),
        ImVec2(center.x - 3.2f, center.y - 2.3f),
        ImVec2(center.x + 0.5f, center.y - 5.5f),
        ImVec2(center.x + 0.5f, center.y + 5.5f)
    };
    dl->AddLine(pts[0], pts[1], col, 1.4f);
    dl->AddLine(pts[1], pts[2], col, 1.4f);
    dl->AddLine(pts[2], pts[3], col, 1.4f);
    dl->AddLine(pts[3], ImVec2(center.x - 3.2f, center.y + 2.3f), col, 1.4f);
    dl->AddLine(ImVec2(center.x - 3.2f, center.y + 2.3f), ImVec2(center.x - 6.5f, center.y + 2.3f), col, 1.4f);
    dl->AddLine(ImVec2(center.x - 6.5f, center.y + 2.3f), pts[0], col, 1.4f);
    if (muted) {
        dl->AddLine(ImVec2(center.x + 3.0f, center.y - 3.0f), ImVec2(center.x + 7.5f, center.y + 3.0f), col, 1.6f);
        dl->AddLine(ImVec2(center.x + 7.5f, center.y - 3.0f), ImVec2(center.x + 3.0f, center.y + 3.0f), col, 1.6f);
    } else {
        dl->PathClear();
        dl->PathArcTo(ImVec2(center.x + 1.0f, center.y), 3.8f, -0.8f, 0.8f, 8);
        dl->PathStroke(col, 0, 1.4f);
        dl->PathClear();
        dl->PathArcTo(ImVec2(center.x + 1.0f, center.y), 6.5f, -0.7f, 0.7f, 8);
        dl->PathStroke(col, 0, 1.3f);
    }
}

void DrawGearIcon(ImDrawList* dl, ImVec2 center, ImU32 col) {
    dl->AddCircle(center, 3.2f, col, 16, 1.5f);
    for (int i = 0; i < 8; ++i) {
        float ang = static_cast<float>(i) * (3.14159265f / 4.0f);
        float c = std::cos(ang);
        float s = std::sin(ang);
        dl->AddLine(
            ImVec2(center.x + c * 4.7f, center.y + s * 4.7f),
            ImVec2(center.x + c * 6.8f, center.y + s * 6.8f),
            col,
            1.6f
        );
    }
}

void DrawFallbackAppGlyph(ImDrawList* dl, ImVec2 box_min, float size, ImU32 col) {
    float pad = size * 0.16f;
    ImVec2 p0(box_min.x + pad, box_min.y + pad);
    ImVec2 p1(box_min.x + size - pad, box_min.y + size - pad);
    dl->AddRect(p0, p1, col, size * 0.20f, 0, 1.4f);
    float mid = size * 0.5f;
    float q = size * 0.14f;
    dl->AddRectFilled(
        ImVec2(box_min.x + mid - q, box_min.y + mid - q),
        ImVec2(box_min.x + mid + q, box_min.y + mid + q),
        col,
        2.0f
    );
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

DexlinkUi::~DexlinkUi() {
    ReleaseIconTextures();
}

void DexlinkUi::ReleaseIconTextures() {
    for (auto& kv : icon_srvs_) {
        if (kv.second) {
            kv.second->Release();
        }
    }
    icon_srvs_.clear();
}

ID3D11ShaderResourceView* DexlinkUi::GetOrCreateAppIcon(ID3D11Device* device, const AndroidAppInfo& app) {
    if (app.package_name.empty()) return nullptr;
    auto it = icon_srvs_.find(app.package_name);
    if (it != icon_srvs_.end()) {
        return it->second;
    }
    if (!device || app.icon_w <= 0 || app.icon_h <= 0 ||
        app.icon_rgba.size() != static_cast<size_t>(app.icon_w * app.icon_h * 4)) {
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(app.icon_w);
    desc.Height = static_cast<UINT>(app.icon_h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init_data{};
    init_data.pSysMem = app.icon_rgba.data();
    init_data.SysMemPitch = static_cast<UINT>(app.icon_w * 4);

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, &init_data, &tex)) || !tex) {
        return nullptr;
    }

    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = device->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    if (FAILED(hr) || !srv) {
        return nullptr;
    }

    icon_srvs_[app.package_name] = srv;
    return srv;
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
    st.WindowRounding = 4.0f;
    st.ChildRounding = 2.0f;
    st.FrameRounding = 3.0f;
    st.PopupRounding = 4.0f;
    st.ScrollbarRounding = 3.0f;
    st.GrabRounding = 2.0f;
    st.WindowBorderSize = 1.0f;
    st.FrameBorderSize = 1.0f;
    st.PopupBorderSize = 1.0f;
    st.WindowPadding = ImVec2(8.0f, 6.0f);
    st.FramePadding = ImVec2(6.0f, 4.0f);
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
    c[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.24f, 0.24f, 0.24f, 1.0f);
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

    // 2. Floating App Drawer Transition (180ms)
    if (cfg_.overview_open && !prev_overview_open_) {
        focus_overview_search_ = true;
        adb_.RefreshAppsAsync();
    }
    prev_overview_open_ = cfg_.overview_open;
    overview_anim_ = StepLinear(overview_anim_, cfg_.overview_open, 0.18f, dt);

    // 3. Quick Settings Popover (150ms)
    popover_anim_ = StepLinear(popover_anim_, cfg_.drawer_open, 0.15f, dt);

    // 4. Bottom Taskbar Auto-Hide in Gaming Mode or Fullscreen (180ms)
    const bool auto_hide = keymapper_.GamingMode() || cfg_.fullscreen;
    const bool near_bottom = !keymapper_.GamingMode() && ImGui::IsMousePosValid(&io.MousePos) &&
                             io.MousePos.y >= (win_h - 10.0f);
    const bool inside_bar = !keymapper_.GamingMode() && ImGui::IsMousePosValid(&io.MousePos) &&
                            io.MousePos.y >= (win_h - kTaskbarHeight - 4.0f) &&
                            io.MousePos.y <= win_h;
    const bool show_bar = !auto_hide || near_bottom || (dock_anim_ > 0.1f && inside_bar) ||
                          cfg_.overview_open || cfg_.drawer_open || cfg_.keymap_edit;
    dock_anim_ = StepLinear(dock_anim_, show_bar && (win_w > 320.0f), 0.18f, dt);
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

    // Zero top bar: 95%+ of screen is pure workspace; flush bottom taskbar reserves 44px in desktop mode
    const bool auto_hide = keymapper_.GamingMode() || cfg_.fullscreen;
    const float reserve_bottom = auto_hide ? 0.0f : kTaskbarHeight;
    const float canvas_w = std::max(win_w, 1.0f);
    const float canvas_h = std::max(win_h - reserve_bottom, 1.0f);

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
    const float base_x = (canvas_w - base_w) * 0.5f;
    const float base_y = (canvas_h - base_h) * 0.5f;
    out_video_rect = VideoRect{base_x, base_y, base_w, base_h};

    const float connect_scale = has_video ? (0.97f + 0.03f * connect_t) : 1.0f;
    const float overview_scale = 1.0f - 0.03f * overview_t;
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
                ImVec2(0.0f, 0.0f),
                ImVec2(win_w, win_h - kTaskbarHeight * dock_t),
                WithAlpha(IM_COL32(0x06, 0x06, 0x08, 0xFF), 0.65f * overview_t)
            );
        }
    } else {
        char wait_buf[220];
        if (!tel.last_error.empty()) {
            std::snprintf(wait_buf, sizeof(wait_buf), "USB: %s  |  %s  |  %s", tel.device_state.c_str(), tel.device_serial.c_str(), tel.last_error.c_str());
        } else if (tel.device_state == "BRIDGE") {
            std::snprintf(wait_buf, sizeof(wait_buf), "USB: BRIDGE  |  %s  |  CONNECTING", tel.device_serial.c_str());
        } else {
            std::snprintf(wait_buf, sizeof(wait_buf), "USB: %s  |  %s", tel.device_state.c_str(), tel.device_serial.c_str());
        }
        bg->AddText(font_mono_, 13.0f, ImVec2(base_x + 18.0f, base_y + 18.0f), tel.last_error.empty() ? kColMuted : kColError, wait_buf);
    }

    if (overview_t > 0.01f) {
        RenderFloatingAppDrawer(renderer.Device(), win_w, win_h, tel);
    }
    if (dock_t > 0.01f) {
        RenderBottomTaskbar(window, renderer.Device(), win_w, win_h, tel, out_quit_requested);
    }
    if (popover_t > 0.01f) {
        RenderQuickSettingsPopover(win_w, win_h, tel);
    }
    if (cfg_.keymap_edit) {
        RenderKeymapOverlay(out_video_rect);
    }
}

void DexlinkUi::RenderBottomTaskbar(
    SDL_Window* window,
    ID3D11Device* device,
    float win_w,
    float win_h,
    const TelemetrySnapshot& tel,
    bool& quit_requested
) {
    const float alpha = EaseOutCubic(dock_anim_);
    const float translate_y = (1.0f - alpha) * (kTaskbarHeight + 4.0f);
    const float bar_y = win_h - kTaskbarHeight + translate_y;

    const auto apps = adb_.GetInstalledApps();
    const std::string active_pkg = adb_.ActivePackage();

    // Build pinned + running real Android apps (no 2-letter text boxes)
    std::vector<DockItem> dock_apps;
    auto push_app = [&](const std::string& pkg, const std::string& fallback_lbl, bool home) {
        for (const auto& existing : dock_apps) {
            if (existing.package_name == pkg) return;
        }
        std::string label = fallback_lbl;
        ID3D11ShaderResourceView* srv = nullptr;
        bool running = (pkg == active_pkg);
        for (const auto& a : apps) {
            if (a.package_name == pkg) {
                if (!a.short_name.empty()) label = a.short_name;
                if (a.is_running) running = true;
                srv = GetOrCreateAppIcon(device, a);
                break;
            }
        }
        dock_apps.push_back({pkg, label, srv, home, pkg == active_pkg, running});
    };

    static const char* kPinnedPkgs[][2] = {
        {"com.android.chrome", "Chrome"},
        {"com.android.vending", "Play Store"},
        {"com.android.settings", "Settings"},
        {"com.google.android.youtube", "YouTube"},
        {"com.discord", "Discord"},
        {"com.tencent.ig", "PUBG Mobile"},
        {"com.mobilelegends.mi", "Mobile Legends"},
        {"com.miHoYo.GenshinImpact", "Genshin Impact"},
        {"com.google.android.apps.photos", "Photos"},
    };
    for (const auto& pair : kPinnedPkgs) {
        bool installed = false;
        bool has_icon = false;
        for (const auto& a : apps) {
            if (a.package_name == pair[0]) {
                installed = true;
                has_icon = !a.icon_rgba.empty();
                break;
            }
        }
        const bool core_pin = (std::string(pair[0]) == "com.android.settings" ||
                               std::string(pair[0]) == "com.android.chrome" ||
                               std::string(pair[0]) == "com.android.vending");
        if (((installed && has_icon) || core_pin) && dock_apps.size() < 7) {
            push_app(pair[0], pair[1], false);
        }
    }
    // Append active/running user apps dynamically
    for (const auto& a : apps) {
        if (dock_apps.size() >= 9) break;
        if (a.package_name == "com.dexlink.app") continue;
        if (a.is_running || a.package_name == active_pkg) {
            push_app(a.package_name, a.short_name, false);
        }
    }
    if (!active_pkg.empty() && active_pkg != "com.dexlink.app" && dock_apps.size() < 10) {
        push_app(active_pkg, active_pkg, false);
    }

    ImGui::SetNextWindowPos(ImVec2(0.0f, bar_y));
    ImGui::SetNextWindowSize(ImVec2(win_w, kTaskbarHeight));

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (ImGui::Begin("##DeXBottomTaskbar", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0001f, 0.05f);

        // #141414 at 92% opacity + 1px solid #2A2A2A top border
        dl->AddRectFilled(ImVec2(0.0f, bar_y), ImVec2(win_w, bar_y + kTaskbarHeight), WithAlpha(kColTaskbar, alpha));
        dl->AddLine(ImVec2(0.0f, bar_y), ImVec2(win_w, bar_y), WithAlpha(kColBorder, alpha), 1.0f);

        auto icon_btn = [&](const char* id, float x, float y, float w, float h, bool active, const char* tooltip) -> std::pair<bool, bool> {
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            ImGui::PushID(id);
            bool clicked = ImGui::InvisibleButton("##btn", ImVec2(w, h));
            bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hovered) {
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), WithAlpha(kColSurfaceHover, alpha), 5.0f);
                if (tooltip && tooltip[0]) {
                    DrawTooltipAboveTaskbar(font_ui_, x + w * 0.5f, bar_y, tooltip);
                }
            } else if (active) {
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), WithAlpha(IM_COL32(0x22, 0x22, 0x22, 0xAA), alpha), 5.0f);
            }
            return {clicked, hovered};
        };

        // ------------------------------------------------------------------
        // ZONE 1 (LEFT): Android Navigation Icons (Recents |||, Home O, Back <)
        // ------------------------------------------------------------------
        const float btn_y = bar_y + 7.0f;
        const float nav_sz = 30.0f;
        float lx = 12.0f;

        if (icon_btn("nav_recents", lx, btn_y, nav_sz, nav_sz, false, "Recents").first) {
            transport_.SendControl({.cmd = protocol::ControlCmd::NavAction, .arg_u8 = 2});
        }
        DrawNavRecentsIcon(dl, ImVec2(lx + nav_sz * 0.5f, btn_y + nav_sz * 0.5f), WithAlpha(kColText, alpha));
        lx += nav_sz + 4.0f;

        const bool home_active = active_pkg.empty() || active_pkg == "com.dexlink.app";
        if (icon_btn("nav_home", lx, btn_y, nav_sz, nav_sz, false, "DeX Home").first) {
            cfg_.overview_open = false;
            transport_.SendControl({.cmd = protocol::ControlCmd::NavAction, .arg_u8 = 1});
            adb_.MarkAppRunning("com.dexlink.app");
        }
        DrawNavHomeIcon(
            dl,
            ImVec2(lx + nav_sz * 0.5f, btn_y + nav_sz * 0.5f),
            WithAlpha(home_active ? kColAccent : kColMuted, alpha)
        );
        lx += nav_sz + 4.0f;

        if (icon_btn("nav_back", lx, btn_y, nav_sz, nav_sz, false, "Back").first) {
            transport_.SendControl({.cmd = protocol::ControlCmd::NavAction, .arg_u8 = 0});
        }
        DrawNavBackIcon(dl, ImVec2(lx + nav_sz * 0.5f, btn_y + nav_sz * 0.5f), WithAlpha(kColText, alpha));
        const float left_end_x = lx + nav_sz + 12.0f;

        // ------------------------------------------------------------------
        // ZONE 3 (RIGHT): System Tray + Telemetry Pill + Clock/Date + Window
        // ------------------------------------------------------------------
        std::time_t now_t = std::time(nullptr);
        std::tm local_tm{};
        localtime_s(&local_tm, &now_t);
        char time_buf[16];
        char date_buf[24];
        std::strftime(time_buf, sizeof(time_buf), "%H:%M", &local_tm);
        std::strftime(date_buf, sizeof(date_buf), "%a, %d/%m", &local_tm);

        float show_fps = (tel.render_fps > 0.5f) ? tel.render_fps : tel.enc_fps;
        char tel_buf[64];
        std::snprintf(tel_buf, sizeof(tel_buf), "%.0f FPS  %ums   %d%%", show_fps, tel.rtt_ms, tel.battery_pct);
        ImVec2 tel_sz = font_mono_->CalcTextSizeA(12.0f, FLT_MAX, 0.0f, tel_buf);
        const float pill_w = tel_sz.x + 26.0f;
        const float clock_w = 72.0f;
        const float win_ctrl_w = cfg_.fullscreen ? 0.0f : 48.0f;
        const float tray_icons_w = 5 * 28.0f + 4 * 4.0f; // GAME, KEYMAP, SCR OFF, AUDIO, SETTINGS
        const float right_total_w = tray_icons_w + 14.0f + pill_w + 10.0f + clock_w + (win_ctrl_w > 0.0f ? (8.0f + win_ctrl_w) : 0.0f) + 12.0f;
        float rx = std::max(left_end_x + 40.0f, win_w - right_total_w);
        const float right_start_x = rx;

        // Tray trigger buttons
        const bool gaming_on = keymapper_.GamingMode();
        if (icon_btn("tray_game", rx, btn_y, 28.0f, nav_sz, gaming_on, gaming_on ? "Game Mode [TAB]" : "Game Mode [TAB]").first) {
            keymapper_.SetGamingMode(!gaming_on);
        }
        DrawGamepadIcon(dl, ImVec2(rx + 14.0f, btn_y + 15.0f), WithAlpha(gaming_on ? kColConnected : kColText, alpha));
        rx += 32.0f;

        if (icon_btn("tray_keymap", rx, btn_y, 28.0f, nav_sz, cfg_.keymap_edit, "Keymap [F1]").first) {
            cfg_.keymap_edit = !cfg_.keymap_edit;
            if (cfg_.keymap_edit && gaming_on) keymapper_.SetGamingMode(false);
        }
        DrawKeymapIcon(dl, ImVec2(rx + 14.0f, btn_y + 15.0f), WithAlpha(cfg_.keymap_edit ? kColConnected : kColText, alpha));
        rx += 32.0f;

        if (icon_btn("tray_scroff", rx, btn_y, 28.0f, nav_sz, cfg_.screen_off, cfg_.screen_off ? "Wake Phone Screen" : "Screen Off").first) {
            cfg_.screen_off = !cfg_.screen_off;
            transport_.SendControl({
                .cmd = protocol::ControlCmd::ScreenOff,
                .arg_u8 = static_cast<uint8_t>(cfg_.screen_off ? 1 : 0),
                .arg_u16 = 0,
                .arg0 = 0,
                .arg1 = 0,
            });
        }
        DrawScreenOffIcon(dl, ImVec2(rx + 14.0f, btn_y + 15.0f), WithAlpha(cfg_.screen_off ? kColConnected : kColText, alpha), cfg_.screen_off);
        rx += 32.0f;

        if (icon_btn("tray_audio", rx, btn_y, 28.0f, nav_sz, cfg_.audio_muted, cfg_.audio_muted ? "Unmute Audio" : "Mute Audio").first) {
            cfg_.audio_muted = !cfg_.audio_muted;
            audio_player_.SetMuted(cfg_.audio_muted);
        }
        DrawAudioIcon(dl, ImVec2(rx + 14.0f, btn_y + 15.0f), WithAlpha(cfg_.audio_muted ? kColError : kColText, alpha), cfg_.audio_muted);
        rx += 32.0f;

        if (icon_btn("tray_settings", rx, btn_y, 28.0f, nav_sz, cfg_.drawer_open, "Quick Settings").first) {
            cfg_.drawer_open = !cfg_.drawer_open;
            if (cfg_.drawer_open) cfg_.overview_open = false;
        }
        DrawGearIcon(dl, ImVec2(rx + 14.0f, btn_y + 15.0f), WithAlpha(cfg_.drawer_open ? kColConnected : kColText, alpha));
        rx += 34.0f;

        // 1px vertical separator
        dl->AddLine(ImVec2(rx, bar_y + 11.0f), ImVec2(rx, bar_y + kTaskbarHeight - 11.0f), WithAlpha(kColBorder, alpha), 1.0f);
        rx += 8.0f;

        // Live telemetry pill
        const float pill_y0 = bar_y + 9.0f;
        const float pill_y1 = bar_y + kTaskbarHeight - 9.0f;
        dl->AddRectFilled(ImVec2(rx, pill_y0), ImVec2(rx + pill_w, pill_y1), WithAlpha(kColPill, alpha), 4.0f);
        dl->AddRect(ImVec2(rx, pill_y0), ImVec2(rx + pill_w, pill_y1), WithAlpha(kColBorder, alpha), 4.0f);
        ImU32 dot_col = kColMuted;
        if (tel.device_state == "ERR" || tel.device_state == "NO ADB" || !tel.last_error.empty()) {
            dot_col = kColError;
        } else if (tel.video_connected) {
            dot_col = kColConnected;
        }
        dl->AddCircleFilled(ImVec2(rx + 10.0f, bar_y + kTaskbarHeight * 0.5f), 3.3f, WithAlpha(dot_col, alpha));
        dl->AddText(
            font_mono_,
            12.0f,
            ImVec2(rx + 18.0f, bar_y + 16.0f),
            WithAlpha(tel.thermal_warn ? kColError : kColText, alpha),
            tel_buf
        );
        rx += pill_w + 10.0f;

        // Clock HH:MM + Date (Thu, 26/11)
        ImVec2 t_sz = font_mono_->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, time_buf);
        ImVec2 d_sz = font_ui_->CalcTextSizeA(11.0f, FLT_MAX, 0.0f, date_buf);
        dl->AddText(
            font_mono_,
            13.0f,
            ImVec2(rx + (clock_w - t_sz.x) * 0.5f, bar_y + 7.0f),
            WithAlpha(kColText, alpha),
            time_buf
        );
        dl->AddText(
            font_ui_,
            11.0f,
            ImVec2(rx + (clock_w - d_sz.x) * 0.5f, bar_y + 23.0f),
            WithAlpha(kColMuted, alpha),
            date_buf
        );
        rx += clock_w + 6.0f;

        if (!cfg_.fullscreen) {
            dl->AddLine(ImVec2(rx, bar_y + 11.0f), ImVec2(rx, bar_y + kTaskbarHeight - 11.0f), WithAlpha(kColBorder, alpha), 1.0f);
            rx += 6.0f;
            if (icon_btn("win_min", rx, btn_y + 2.0f, 22.0f, 26.0f, false, "Minimize").first) {
                SDL_MinimizeWindow(window);
            }
            dl->AddLine(ImVec2(rx + 6.0f, bar_y + 22.0f), ImVec2(rx + 16.0f, bar_y + 22.0f), WithAlpha(kColMuted, alpha), 1.5f);
            rx += 24.0f;

            if (icon_btn("win_close", rx, btn_y + 2.0f, 22.0f, 26.0f, false, "Close").first) {
                quit_requested = true;
            }
            dl->AddLine(ImVec2(rx + 7.0f, bar_y + 17.0f), ImVec2(rx + 15.0f, bar_y + 26.0f), WithAlpha(kColMuted, alpha), 1.5f);
            dl->AddLine(ImVec2(rx + 15.0f, bar_y + 17.0f), ImVec2(rx + 7.0f, bar_y + 26.0f), WithAlpha(kColMuted, alpha), 1.5f);
        }

        // ------------------------------------------------------------------
        // ZONE 2 (CENTER): Search + 9-dot App Drawer + Real 28x28px App Icons
        // ------------------------------------------------------------------
        const float center_zone_w = std::max(0.0f, right_start_x - left_end_x);
        const float app_btn_sz = 34.0f;
        const float app_icon_sz = 28.0f;
        const float app_gap = 6.0f;
        const float fixed_center_w = 30.0f + 4.0f + 30.0f + 13.0f; // Search + 9-dot + divider
        int max_visible_apps = 0;
        if (center_zone_w > fixed_center_w + app_btn_sz) {
            max_visible_apps = static_cast<int>((center_zone_w - fixed_center_w + app_gap) / (app_btn_sz + app_gap));
        }
        const int draw_app_count = std::min(static_cast<int>(dock_apps.size()), max_visible_apps);
        const float total_center_w = fixed_center_w +
                                     (draw_app_count > 0 ? draw_app_count * app_btn_sz + (draw_app_count - 1) * app_gap : 0.0f);
        float cx = left_end_x + std::max(0.0f, (center_zone_w - total_center_w) * 0.5f);

        // Search icon
        if (icon_btn("center_search", cx, btn_y, 30.0f, nav_sz, false, "Search Apps").first) {
            cfg_.drawer_open = false;
            cfg_.overview_open = true;
            focus_overview_search_ = true;
            if (gaming_on) keymapper_.SetGamingMode(false);
        }
        DrawSearchIcon(dl, ImVec2(cx + 15.0f, btn_y + 15.0f), WithAlpha(kColText, alpha));
        cx += 34.0f;

        // 9-dot App Drawer icon
        if (icon_btn("center_drawer", cx, btn_y, 30.0f, nav_sz, cfg_.overview_open, "App Drawer [Super]").first) {
            cfg_.drawer_open = false;
            cfg_.overview_open = !cfg_.overview_open;
            if (cfg_.overview_open && gaming_on) keymapper_.SetGamingMode(false);
        }
        DrawNineDotIcon(dl, ImVec2(cx + 15.0f, btn_y + 15.0f), WithAlpha(cfg_.overview_open ? kColConnected : kColText, alpha));
        cx += 36.0f;

        // Subtle divider before app icons
        dl->AddLine(ImVec2(cx, bar_y + 11.0f), ImVec2(cx, bar_y + kTaskbarHeight - 11.0f), WithAlpha(kColBorder, alpha), 1.0f);
        cx += 7.0f;

        for (int i = 0; i < draw_app_count; ++i) {
            const auto& it = dock_apps[i];
            const float bx = cx + i * (app_btn_sz + app_gap);
            const float by = bar_y + 5.0f;

            ImGui::SetCursorScreenPos(ImVec2(bx, by));
            ImGui::PushID(1000 + i);
            bool clicked = ImGui::InvisibleButton("##dockapp", ImVec2(app_btn_sz, 34.0f));
            bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();

            const size_t anim_idx = std::min(static_cast<size_t>(i), dock_hover_anim_.size() - 1);
            dock_hover_anim_[anim_idx] = StepLinear(dock_hover_anim_[anim_idx], hovered, 0.10f, dt);
            const float lift_y = -2.0f * EaseOutCubic(dock_hover_anim_[anim_idx]);

            if (hovered) {
                dl->AddRectFilled(
                    ImVec2(bx, by),
                    ImVec2(bx + app_btn_sz, by + 34.0f),
                    WithAlpha(kColSurfaceHover, alpha),
                    5.0f
                );
                DrawTooltipAboveTaskbar(font_ui_, bx + app_btn_sz * 0.5f, bar_y, it.label.c_str());
            }

            const ImVec2 icon_min(bx + (app_btn_sz - app_icon_sz) * 0.5f, by + 2.0f + lift_y);
            const ImVec2 icon_max(icon_min.x + app_icon_sz, icon_min.y + app_icon_sz);
            if (it.icon_srv) {
                dl->AddImageRounded(
                    reinterpret_cast<ImTextureID>(it.icon_srv),
                    icon_min,
                    icon_max,
                    ImVec2(0, 0),
                    ImVec2(1, 1),
                    WithAlpha(IM_COL32(0xFF, 0xFF, 0xFF, 0xFF), alpha),
                    5.0f
                );
            } else {
                DrawFallbackAppGlyph(dl, icon_min, app_icon_sz, WithAlpha(it.is_active ? kColConnected : kColText, alpha));
            }

            // Subtle 2px underline indicator underneath active/running app icon
            if (it.is_active || it.is_running) {
                const float ind_w = it.is_active ? 14.0f : 7.0f;
                const float ind_x = bx + (app_btn_sz - ind_w) * 0.5f;
                const float ind_y = bar_y + kTaskbarHeight - 4.0f;
                dl->AddRectFilled(
                    ImVec2(ind_x, ind_y),
                    ImVec2(ind_x + ind_w, ind_y + 2.0f),
                    WithAlpha(it.is_active ? kColConnected : kColAccent, alpha),
                    1.0f
                );
            }

            if (clicked) {
                cfg_.overview_open = false;
                cfg_.drawer_open = false;
                LaunchAndroidApp(it.package_name, tel);
            }
        }

        // Frameless window drag on empty taskbar background
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
    ImGui::PopStyleVar(4);
}

void DexlinkUi::RenderFloatingAppDrawer(ID3D11Device* device, float win_w, float win_h, const TelemetrySnapshot& tel) {
    const float t = EaseOutCubic(overview_anim_);
    const float scale = 0.94f + 0.06f * t;
    const float avail_h = std::max(win_h - kTaskbarHeight, 240.0f);
    const float panel_w = std::clamp(win_w * 0.80f * scale, 320.0f, win_w - 32.0f);
    const float panel_h = std::clamp(avail_h * 0.75f * scale, 240.0f, avail_h - 32.0f);
    const float panel_x = (win_w - panel_w) * 0.5f;
    const float panel_y = (avail_h - panel_h) * 0.5f;

    ImGui::SetNextWindowPos(ImVec2(panel_x, panel_y));
    ImGui::SetNextWindowSize(ImVec2(panel_w, panel_h));

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, t);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f, 18.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, U32ToVec4(IM_COL32(0x14, 0x14, 0x14, 0xF5)));
    ImGui::PushStyleColor(ImGuiCol_Border, U32ToVec4(kColBorder));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("##FloatingAppDrawer", nullptr, flags)) {
        // Click outside panel (above taskbar) closes App Drawer immediately
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (cfg_.overview_open &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            ImGui::IsMousePosValid(&mouse)) {
            const bool inside_panel = (mouse.x >= panel_x && mouse.x <= panel_x + panel_w &&
                                       mouse.y >= panel_y && mouse.y <= panel_y + panel_h);
            const bool on_taskbar = (mouse.y >= win_h - kTaskbarHeight);
            if (!inside_panel && !on_taskbar) {
                cfg_.overview_open = false;
            }
        }

        // Centered clean search bar at top
        const float search_w = std::min(420.0f, panel_w - 80.0f);
        const float search_offset_x = std::max(0.0f, (panel_w - 44.0f - (search_w + 34.0f)) * 0.5f);
        ImGui::SetCursorPosX(22.0f + search_offset_x);

        ImGui::PushFont(font_ui_);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 6.0f));
        ImGui::SetNextItemWidth(search_w);
        if (focus_overview_search_) {
            ImGui::SetKeyboardFocusHere();
            focus_overview_search_ = false;
        }
        ImGui::InputTextWithHint("##DrawerSearch", "Search apps...", overview_search_buf_, sizeof(overview_search_buf_));
        ImGui::PopStyleVar(2);
        ImGui::PopFont();

        ImGui::SameLine(0, 8.0f);
        if (ImGui::Button("Refresh", ImVec2(58.0f, 28.0f))) {
            adb_.RefreshAppsAsync();
        }

        ImGui::Dummy(ImVec2(0.0f, 10.0f));

        const auto all_apps = adb_.GetInstalledApps();
        const std::string active_pkg = adb_.ActivePackage();
        const std::string query = ToLowerAscii(overview_search_buf_);

        std::vector<AndroidAppInfo> filtered;
        filtered.reserve(all_apps.size());
        for (const auto& app : all_apps) {
            if (!query.empty()) {
                if (ToLowerAscii(app.short_name).find(query) == std::string::npos) {
                    continue;
                }
            }
            filtered.push_back(app);
        }

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        if (ImGui::BeginChild("##AppDrawerGrid", ImVec2(0, 0), false)) {
            if (filtered.empty()) {
                ImGui::PushFont(font_ui_);
                ImGui::TextColored(
                    U32ToVec4(kColMuted),
                    tel.usb_attached ? "No matching apps" : "Connect device over USB to load apps"
                );
                ImGui::PopFont();
            } else {
                const float inner_w = ImGui::GetContentRegionAvail().x;
                const float cell_w = 104.0f;
                const float cell_h = 94.0f;
                const float gap_x = 10.0f;
                const int cols = std::max(2, static_cast<int>((inner_w + gap_x) / (cell_w + gap_x)));
                const float row_w = cols * cell_w + (cols - 1) * gap_x;
                const float start_offset = std::max(0.0f, (inner_w - row_w) * 0.5f);
                ImDrawList* dl = ImGui::GetWindowDrawList();

                for (size_t i = 0; i < filtered.size(); ++i) {
                    const int col = static_cast<int>(i % cols);
                    if (col == 0) {
                        if (start_offset > 0.5f) {
                            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + start_offset);
                        }
                    } else {
                        ImGui::SameLine(0, gap_x);
                    }

                    const auto& app = filtered[i];
                    const bool is_active = (app.package_name == active_pkg);
                    const ImVec2 pos = ImGui::GetCursorScreenPos();

                    ImGui::PushID(static_cast<int>(i));
                    bool clicked = ImGui::InvisibleButton("##drawerapp", ImVec2(cell_w, cell_h));
                    bool hovered = ImGui::IsItemHovered();
                    ImGui::PopID();

                    // Icon-only cell: NO heavy card borders; soft highlight only on hover
                    if (hovered) {
                        dl->AddRectFilled(pos, ImVec2(pos.x + cell_w, pos.y + cell_h), WithAlpha(kColSurfaceHover, t), 8.0f);
                    }

                    const float icon_sz = 48.0f;
                    const ImVec2 icon_min(pos.x + (cell_w - icon_sz) * 0.5f, pos.y + 10.0f);
                    const ImVec2 icon_max(icon_min.x + icon_sz, icon_min.y + icon_sz);
                    ID3D11ShaderResourceView* srv = GetOrCreateAppIcon(device, app);
                    if (srv) {
                        dl->AddImageRounded(
                            reinterpret_cast<ImTextureID>(srv),
                            icon_min,
                            icon_max,
                            ImVec2(0, 0),
                            ImVec2(1, 1),
                            WithAlpha(IM_COL32(0xFF, 0xFF, 0xFF, 0xFF), t),
                            8.0f
                        );
                    } else {
                        DrawFallbackAppGlyph(dl, icon_min, icon_sz, WithAlpha(is_active ? kColConnected : kColText, t));
                    }

                    // 1-line App Name underneath (NO package names)
                    const std::string lbl = TruncateTextToWidth(font_ui_, 13.0f, app.short_name, cell_w - 10.0f);
                    ImVec2 lsz = font_ui_->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, lbl.c_str());
                    dl->AddText(
                        font_ui_,
                        13.0f,
                        ImVec2(pos.x + (cell_w - lsz.x) * 0.5f, pos.y + 66.0f),
                        WithAlpha(kColText, t),
                        lbl.c_str()
                    );

                    if (app.is_running || is_active) {
                        dl->AddRectFilled(
                            ImVec2(pos.x + (cell_w - 14.0f) * 0.5f, pos.y + cell_h - 5.0f),
                            ImVec2(pos.x + (cell_w + 14.0f) * 0.5f, pos.y + cell_h - 3.0f),
                            WithAlpha(is_active ? kColConnected : kColAccent, t),
                            1.0f
                        );
                    }

                    if (clicked) {
                        cfg_.overview_open = false;
                        LaunchAndroidApp(app.package_name, tel);
                    }
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
}

void DexlinkUi::RenderQuickSettingsPopover(float win_w, float win_h, const TelemetrySnapshot& tel) {
    const float alpha = EaseOutCubic(popover_anim_);
    const float w = 240.0f;
    const float slide_y = 8.0f * (1.0f - alpha);
    const float x = std::max(8.0f, win_w - w - 8.0f);
    const float y = std::max(8.0f, win_h - kTaskbarHeight - 8.0f - quick_settings_h_ + slide_y);

    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 120.0f), ImVec2(w, win_h - kTaskbarHeight - 16.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, U32ToVec4(kColSurface));
    ImGui::PushStyleColor(ImGuiCol_Border, U32ToVec4(kColBorder));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("##QuickSettingsPopover", nullptr, flags)) {
        quick_settings_h_ = ImGui::GetWindowHeight();

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (cfg_.drawer_open &&
            !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            ImGui::IsMousePosValid(&mouse)) {
            const ImVec2 wpos = ImGui::GetWindowPos();
            const ImVec2 wsz = ImGui::GetWindowSize();
            const bool inside = (mouse.x >= wpos.x && mouse.x <= wpos.x + wsz.x &&
                                 mouse.y >= wpos.y && mouse.y <= wpos.y + wsz.y);
            const bool on_taskbar = (mouse.y >= win_h - kTaskbarHeight);
            if (!inside && !on_taskbar) {
                cfg_.drawer_open = false;
            }
        }

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

        if (ImGui::Button(tel.audio_connected ? "AUDIO: ON" : "GRANT AUDIO", ImVec2(106.0f, 22.0f))) {
            adb_.LaunchCompanionApp();
        }
        ImGui::SameLine(0, 6);
        if (ImGui::Button(cfg_.fullscreen ? "WINDOWED" : "FULLSCREEN", ImVec2(108.0f, 22.0f))) {
            cfg_.fullscreen = !cfg_.fullscreen;
        }

        ImGui::Separator();

        ImGui::PushFont(font_mono_);
        ImGui::Text("DROP %llu  RX %.1fM", static_cast<unsigned long long>(tel.dropped_frames), tel.rx_mbps);
        ImGui::Text("%.0fC  STATE %s", tel.temp_c, tel.device_state.c_str());
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
