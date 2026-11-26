#include "input/Keymapper.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <windows.h>
#include <xinput.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace dexlink {

namespace {

constexpr int kMouseLeftCode = -1;
constexpr int kMouseRightCode = -2;
constexpr int kMouseMiddleCode = -3;
constexpr int kMouseX1Code = -4;
constexpr int kMouseX2Code = -5;

constexpr int kPointerDirect = 0;
constexpr int kPointerJoystick = 1;
constexpr int kPointerMouseLook = 2;

int SdlButtonToInputCode(uint8_t button) {
    switch (button) {
        case SDL_BUTTON_LEFT: return kMouseLeftCode;
        case SDL_BUTTON_RIGHT: return kMouseRightCode;
        case SDL_BUTTON_MIDDLE: return kMouseMiddleCode;
        case SDL_BUTTON_X1: return kMouseX1Code;
        case SDL_BUTTON_X2: return kMouseX2Code;
        default: return 0;
    }
}

int SdlModToAndroidMeta(Uint16 mod) {
    int meta = 0;
    if (mod & KMOD_LSHIFT) meta |= 0x00000040 | 0x00000001;
    if (mod & KMOD_RSHIFT) meta |= 0x00000080 | 0x00000001;
    if (mod & KMOD_LCTRL) meta |= 0x00002000 | 0x00001000;
    if (mod & KMOD_RCTRL) meta |= 0x00004000 | 0x00001000;
    if (mod & KMOD_LALT) meta |= 0x00000010 | 0x00000002;
    if (mod & KMOD_RALT) meta |= 0x00000020 | 0x00000002;
    return meta;
}

}  // namespace

Keymapper::Keymapper(UsbTransport& transport) : transport_(transport) {
    xinput_dll_ = LoadLibraryA("xinput1_4.dll");
    if (!xinput_dll_) xinput_dll_ = LoadLibraryA("xinput9_1_0.dll");
    if (xinput_dll_) {
        void* sym = reinterpret_cast<void*>(GetProcAddress(xinput_dll_, "XInputGetState"));
        xinput_get_state_ = reinterpret_cast<XInputGetStateFn>(sym);
    }

    // Populate clean default FPS/RPG profile
    profile_.taps = {
        {"FIRE", kMouseLeftCode, 0.85f, 0.72f},
        {"AIM", kMouseRightCode, 0.90f, 0.45f},
        {"JUMP", SDL_SCANCODE_SPACE, 0.91f, 0.62f},
        {"CROUCH", SDL_SCANCODE_C, 0.91f, 0.82f},
        {"RELOAD", SDL_SCANCODE_R, 0.77f, 0.85f},
        {"USE", SDL_SCANCODE_F, 0.68f, 0.65f},
        {"BAG", SDL_SCANCODE_B, 0.08f, 0.85f},
    };
}

Keymapper::~Keymapper() {
    ReleaseAll();
    if (xinput_dll_) {
        FreeLibrary(xinput_dll_);
        xinput_dll_ = nullptr;
    }
}

void Keymapper::SetProfileDirectory(const std::string& dir) {
    profile_dir_ = dir;
}

std::vector<std::string> Keymapper::ListProfiles() const {
    std::vector<std::string> files;
    std::error_code ec;
    if (profile_dir_.empty() || !fs::exists(profile_dir_, ec)) {
        return {"default.json"};
    }
    for (const auto& entry : fs::directory_iterator(profile_dir_, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            files.push_back(entry.path().filename().string());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) files.push_back("default.json");
    return files;
}

bool Keymapper::LoadProfile(const std::string& filename) {
    if (profile_dir_.empty()) return false;
    fs::path p = fs::path(profile_dir_) / filename;
    std::ifstream ifs(p);
    if (!ifs.is_open()) return false;

    try {
        json j;
        ifs >> j;
        KeymapProfile loaded;
        loaded.name = j.value("name", filename);
        loaded.package_name = j.value("package_name", "");

        if (j.contains("joystick")) {
            const auto& jj = j["joystick"];
            loaded.joystick.enabled = jj.value("enabled", true);
            loaded.joystick.center_x = jj.value("center_x", 0.16f);
            loaded.joystick.center_y = jj.value("center_y", 0.72f);
            loaded.joystick.radius_x = jj.value("radius_x", 0.08f);
            loaded.joystick.radius_y = jj.value("radius_y", 0.14f);
            loaded.joystick.key_up = jj.value("key_up", static_cast<int>(SDL_SCANCODE_W));
            loaded.joystick.key_down = jj.value("key_down", static_cast<int>(SDL_SCANCODE_S));
            loaded.joystick.key_left = jj.value("key_left", static_cast<int>(SDL_SCANCODE_A));
            loaded.joystick.key_right = jj.value("key_right", static_cast<int>(SDL_SCANCODE_D));
        }

        if (j.contains("mouse_look")) {
            const auto& mj = j["mouse_look"];
            loaded.mouse_look.enabled = mj.value("enabled", true);
            loaded.mouse_look.center_x = mj.value("center_x", 0.65f);
            loaded.mouse_look.center_y = mj.value("center_y", 0.50f);
            loaded.mouse_look.sens_x = mj.value("sens_x", 0.0014f);
            loaded.mouse_look.sens_y = mj.value("sens_y", 0.0022f);
            loaded.mouse_look.bound_half_w = mj.value("bound_half_w", 0.20f);
            loaded.mouse_look.bound_half_h = mj.value("bound_half_h", 0.25f);
            loaded.mouse_look.idle_reset_ms = mj.value("idle_reset_ms", 220);
        }

        if (j.contains("gamepad")) {
            const auto& gj = j["gamepad"];
            loaded.gamepad.enabled = gj.value("enabled", true);
            loaded.gamepad.right_stick_sens = gj.value("right_stick_sens", 0.015f);
            loaded.gamepad.a_x = gj.value("a_x", 0.88f);
            loaded.gamepad.a_y = gj.value("a_y", 0.78f);
            loaded.gamepad.b_x = gj.value("b_x", 0.88f);
            loaded.gamepad.b_y = gj.value("b_y", 0.88f);
            loaded.gamepad.x_x = gj.value("x_x", 0.78f);
            loaded.gamepad.x_y = gj.value("x_y", 0.82f);
            loaded.gamepad.y_x = gj.value("y_x", 0.50f);
            loaded.gamepad.y_y = gj.value("y_y", 0.88f);
            loaded.gamepad.rb_x = gj.value("rb_x", 0.85f);
            loaded.gamepad.rb_y = gj.value("rb_y", 0.60f);
            loaded.gamepad.lb_x = gj.value("lb_x", 0.15f);
            loaded.gamepad.lb_y = gj.value("lb_y", 0.45f);
        }

        if (j.contains("taps") && j["taps"].is_array()) {
            for (const auto& tj : j["taps"]) {
                TapBinding tb;
                tb.label = tj.value("label", "TAP");
                tb.input_code = tj.value("input_code", 0);
                tb.norm_x = tj.value("x", 0.5f);
                tb.norm_y = tj.value("y", 0.5f);
                loaded.taps.push_back(std::move(tb));
            }
        }

        ReleaseAll();
        profile_ = std::move(loaded);
        return true;
    } catch (...) {
        return false;
    }
}

bool Keymapper::SaveProfile(const std::string& filename) const {
    if (profile_dir_.empty()) return false;
    std::error_code ec;
    fs::create_directories(profile_dir_, ec);

    json j;
    j["name"] = profile_.name;
    j["package_name"] = profile_.package_name;
    j["joystick"] = {
        {"enabled", profile_.joystick.enabled},
        {"center_x", profile_.joystick.center_x},
        {"center_y", profile_.joystick.center_y},
        {"radius_x", profile_.joystick.radius_x},
        {"radius_y", profile_.joystick.radius_y},
        {"key_up", profile_.joystick.key_up},
        {"key_down", profile_.joystick.key_down},
        {"key_left", profile_.joystick.key_left},
        {"key_right", profile_.joystick.key_right},
    };
    j["mouse_look"] = {
        {"enabled", profile_.mouse_look.enabled},
        {"center_x", profile_.mouse_look.center_x},
        {"center_y", profile_.mouse_look.center_y},
        {"sens_x", profile_.mouse_look.sens_x},
        {"sens_y", profile_.mouse_look.sens_y},
        {"bound_half_w", profile_.mouse_look.bound_half_w},
        {"bound_half_h", profile_.mouse_look.bound_half_h},
        {"idle_reset_ms", profile_.mouse_look.idle_reset_ms},
    };
    j["gamepad"] = {
        {"enabled", profile_.gamepad.enabled},
        {"right_stick_sens", profile_.gamepad.right_stick_sens},
        {"a_x", profile_.gamepad.a_x}, {"a_y", profile_.gamepad.a_y},
        {"b_x", profile_.gamepad.b_x}, {"b_y", profile_.gamepad.b_y},
        {"x_x", profile_.gamepad.x_x}, {"x_y", profile_.gamepad.x_y},
        {"y_x", profile_.gamepad.y_x}, {"y_y", profile_.gamepad.y_y},
        {"rb_x", profile_.gamepad.rb_x}, {"rb_y", profile_.gamepad.rb_y},
        {"lb_x", profile_.gamepad.lb_x}, {"lb_y", profile_.gamepad.lb_y},
    };
    j["taps"] = json::array();
    for (const auto& t : profile_.taps) {
        j["taps"].push_back({
            {"label", t.label},
            {"input_code", t.input_code},
            {"x", t.norm_x},
            {"y", t.norm_y},
        });
    }

    std::ofstream ofs(fs::path(profile_dir_) / filename);
    if (!ofs.is_open()) return false;
    ofs << j.dump(2) << '\n';
    return true;
}

void Keymapper::SetGamingMode(bool enabled) {
    if (gaming_mode_ == enabled) return;
    ReleaseAll();
    gaming_mode_ = enabled;
    SDL_SetRelativeMouseMode(enabled ? SDL_TRUE : SDL_FALSE);
}

void Keymapper::ReleaseAll(bool direct_only) {
    if (direct_mouse_down_) {
        transport_.SendTouch({kPointerDirect, protocol::TouchAction::Up, 0.5f, 0.5f, 0.0f});
        direct_mouse_down_ = false;
    }
    if (direct_only) return;

    joy_wasd_up_ = joy_wasd_down_ = joy_wasd_left_ = joy_wasd_right_ = false;
    if (joy_active_) {
        transport_.SendTouch({kPointerJoystick, protocol::TouchAction::Up, joy_x_, joy_y_, 0.0f});
        joy_active_ = false;
    }
    ReleaseMouseLook();
    for (size_t i = 0; i < tap_active_.size(); ++i) {
        if (tap_active_[i]) {
            tap_active_[i] = false;
            float nx = (i < profile_.taps.size()) ? profile_.taps[i].norm_x : 0.5f;
            float ny = (i < profile_.taps.size()) ? profile_.taps[i].norm_y : 0.5f;
            transport_.SendTouch({kTapPointerBase + static_cast<int>(i), protocol::TouchAction::Up, nx, ny, 0.0f});
        }
    }
}

bool Keymapper::ScreenToNorm(float sx, float sy, const VideoRect& vr, float& nx, float& ny) const {
    if (vr.w <= 1.0f || vr.h <= 1.0f) return false;
    if (sx < vr.x || sx > vr.x + vr.w || sy < vr.y || sy > vr.y + vr.h) return false;
    nx = std::clamp((sx - vr.x) / vr.w, 0.0f, 1.0f);
    ny = std::clamp((sy - vr.y) / vr.h, 0.0f, 1.0f);
    return true;
}

void Keymapper::HandleSdlEvent(const SDL_Event& ev, const VideoRect& vrect, bool ui_wants_mouse, bool ui_wants_kbd) {
    if (!gaming_mode_) {
        // Direct Mode (Desktop / DeX cursor & keyboard passthrough)
        if (!ui_wants_mouse) {
            if (ev.type == SDL_MOUSEBUTTONDOWN) {
                if (ev.button.button == SDL_BUTTON_MIDDLE) {
                    // Middle-click (or Tab / GAME button) toggles cursor lock for FPS gaming
                    float nx = 0, ny = 0;
                    if (ScreenToNorm(static_cast<float>(ev.button.x), static_cast<float>(ev.button.y), vrect, nx, ny)) {
                        SetGamingMode(true);
                        return;
                    }
                } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                    // Right-click sends Android BACK in direct desktop mode
                    float nx = 0, ny = 0;
                    if (ScreenToNorm(static_cast<float>(ev.button.x), static_cast<float>(ev.button.y), vrect, nx, ny)) {
                        transport_.SendKey({4 /* KEYCODE_BACK */, protocol::KeyAction::Down, 0});
                        transport_.SendKey({4 /* KEYCODE_BACK */, protocol::KeyAction::Up, 0});
                        return;
                    }
                } else if (ev.button.button == SDL_BUTTON_LEFT) {
                    float nx = 0, ny = 0;
                    if (ScreenToNorm(static_cast<float>(ev.button.x), static_cast<float>(ev.button.y), vrect, nx, ny)) {
                        direct_mouse_down_ = true;
                        transport_.SendTouch({kPointerDirect, protocol::TouchAction::Down, nx, ny, 1.0f});
                    }
                }
            } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
                if (direct_mouse_down_) {
                    float nx = std::clamp((static_cast<float>(ev.button.x) - vrect.x) / std::max(vrect.w, 1.0f), 0.0f, 1.0f);
                    float ny = std::clamp((static_cast<float>(ev.button.y) - vrect.y) / std::max(vrect.h, 1.0f), 0.0f, 1.0f);
                    direct_mouse_down_ = false;
                    transport_.SendTouch({kPointerDirect, protocol::TouchAction::Up, nx, ny, 0.0f});
                }
            } else if (ev.type == SDL_MOUSEMOTION) {
                float nx = 0, ny = 0;
                if (direct_mouse_down_) {
                    nx = std::clamp((static_cast<float>(ev.motion.x) - vrect.x) / std::max(vrect.w, 1.0f), 0.0f, 1.0f);
                    ny = std::clamp((static_cast<float>(ev.motion.y) - vrect.y) / std::max(vrect.h, 1.0f), 0.0f, 1.0f);
                    transport_.SendTouch({kPointerDirect, protocol::TouchAction::Move, nx, ny, 1.0f});
                } else if (ScreenToNorm(static_cast<float>(ev.motion.x), static_cast<float>(ev.motion.y), vrect, nx, ny)) {
                    transport_.SendTouch({kPointerDirect, protocol::TouchAction::HoverMove, nx, ny, 0.0f});
                }
            }
        }

        if (!ui_wants_kbd && (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) && !ev.key.repeat) {
            int android_key = SdlScancodeToAndroidKeycode(ev.key.keysym.scancode);
            if (android_key > 0) {
                protocol::KeyEventPayload kp{
                    .keycode = android_key,
                    .action = (ev.type == SDL_KEYDOWN) ? protocol::KeyAction::Down : protocol::KeyAction::Up,
                    .meta_state = SdlModToAndroidMeta(ev.key.keysym.mod),
                };
                transport_.SendKey(kp);
            }
        }
        return;
    }

    // Gaming Mode active (relative mouse locked + WASD joystick + key-to-tap)
    if (ev.type == SDL_MOUSEMOTION) {
        if (profile_.mouse_look.enabled) {
            ProcessMouseLookDelta(
                static_cast<float>(ev.motion.xrel),
                static_cast<float>(ev.motion.yrel),
                SDL_GetTicks()
            );
        }
    } else if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) {
        const bool down = (ev.type == SDL_MOUSEBUTTONDOWN);
        const int code = SdlButtonToInputCode(ev.button.button);
        if (code != 0) {
            bool bound = false;
            for (const auto& t : profile_.taps) {
                if (t.input_code == code) {
                    bound = true;
                    break;
                }
            }
            if (bound) {
                TriggerTap(code, down);
            } else if (code == kMouseRightCode && down) {
                // If RMB has no tap binding, allow RMB to unlock Gaming Mode too
                SetGamingMode(false);
            }
        }
    } else if ((ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) && !ev.key.repeat) {
        const bool down = (ev.type == SDL_KEYDOWN);
        const int sc = static_cast<int>(ev.key.keysym.scancode);

        if (profile_.joystick.enabled) {
            bool joy_changed = false;
            if (sc == profile_.joystick.key_up) { joy_wasd_up_ = down; joy_changed = true; }
            else if (sc == profile_.joystick.key_down) { joy_wasd_down_ = down; joy_changed = true; }
            else if (sc == profile_.joystick.key_left) { joy_wasd_left_ = down; joy_changed = true; }
            else if (sc == profile_.joystick.key_right) { joy_wasd_right_ = down; joy_changed = true; }
            if (joy_changed) {
                UpdateJoystickTouch();
                return;
            }
        }

        TriggerTap(sc, down);
    }
}

void Keymapper::UpdateJoystickTouch() {
    float dx = (joy_wasd_right_ ? 1.0f : 0.0f) - (joy_wasd_left_ ? 1.0f : 0.0f);
    float dy = (joy_wasd_down_ ? 1.0f : 0.0f) - (joy_wasd_up_ ? 1.0f : 0.0f);

    if (dx == 0.0f && dy == 0.0f) {
        if (joy_active_) {
            transport_.SendTouch({kPointerJoystick, protocol::TouchAction::Up, joy_x_, joy_y_, 0.0f});
            joy_active_ = false;
        }
        return;
    }

    float len = std::sqrt(dx * dx + dy * dy);
    dx /= len;
    dy /= len;

    const float target_x = std::clamp(profile_.joystick.center_x + dx * profile_.joystick.radius_x, 0.01f, 0.99f);
    const float target_y = std::clamp(profile_.joystick.center_y + dy * profile_.joystick.radius_y, 0.01f, 0.99f);

    if (!joy_active_) {
        joy_active_ = true;
        joy_x_ = profile_.joystick.center_x;
        joy_y_ = profile_.joystick.center_y;
        transport_.SendTouch({kPointerJoystick, protocol::TouchAction::Down, joy_x_, joy_y_, 1.0f});
    }
    joy_x_ = target_x;
    joy_y_ = target_y;
    transport_.SendTouch({kPointerJoystick, protocol::TouchAction::Move, joy_x_, joy_y_, 1.0f});
}

void Keymapper::ProcessMouseLookDelta(float dx, float dy, uint32_t now_ms) {
    if (dx == 0.0f && dy == 0.0f) return;

    const auto& ml = profile_.mouse_look;
    if (!look_active_) {
        look_active_ = true;
        look_cur_x_ = ml.center_x;
        look_cur_y_ = ml.center_y;
        transport_.SendTouch({kPointerMouseLook, protocol::TouchAction::Down, look_cur_x_, look_cur_y_, 1.0f});
    }

    look_cur_x_ += dx * ml.sens_x;
    look_cur_y_ += dy * ml.sens_y;
    look_last_move_ms_ = now_ms;

    const float min_x = std::max(0.05f, ml.center_x - ml.bound_half_w);
    const float max_x = std::min(0.95f, ml.center_x + ml.bound_half_w);
    const float min_y = std::max(0.05f, ml.center_y - ml.bound_half_h);
    const float max_y = std::min(0.95f, ml.center_y + ml.bound_half_h);

    if (look_cur_x_ < min_x || look_cur_x_ > max_x || look_cur_y_ < min_y || look_cur_y_ > max_y) {
        // Edge-reset touch finger seamlessly back to camera center
        float edge_x = std::clamp(look_cur_x_, min_x, max_x);
        float edge_y = std::clamp(look_cur_y_, min_y, max_y);
        transport_.SendTouch({kPointerMouseLook, protocol::TouchAction::Move, edge_x, edge_y, 1.0f});
        transport_.SendTouch({kPointerMouseLook, protocol::TouchAction::Up, edge_x, edge_y, 0.0f});

        look_cur_x_ = std::clamp(ml.center_x + dx * ml.sens_x, min_x, max_x);
        look_cur_y_ = std::clamp(ml.center_y + dy * ml.sens_y, min_y, max_y);
        transport_.SendTouch({kPointerMouseLook, protocol::TouchAction::Down, ml.center_x, ml.center_y, 1.0f});
    }

    transport_.SendTouch({kPointerMouseLook, protocol::TouchAction::Move, look_cur_x_, look_cur_y_, 1.0f});
}

void Keymapper::ReleaseMouseLook() {
    if (look_active_) {
        transport_.SendTouch({kPointerMouseLook, protocol::TouchAction::Up, look_cur_x_, look_cur_y_, 0.0f});
        look_active_ = false;
        look_cur_x_ = profile_.mouse_look.center_x;
        look_cur_y_ = profile_.mouse_look.center_y;
    }
}

void Keymapper::TriggerTap(int input_code, bool down) {
    const size_t limit = std::min(profile_.taps.size(), static_cast<size_t>(kMaxTapPointers));
    for (size_t i = 0; i < limit; ++i) {
        if (profile_.taps[i].input_code != input_code) continue;
        const int pid = kTapPointerBase + static_cast<int>(i);
        if (down && !tap_active_[i]) {
            tap_active_[i] = true;
            transport_.SendTouch({pid, protocol::TouchAction::Down, profile_.taps[i].norm_x, profile_.taps[i].norm_y, 1.0f});
        } else if (!down && tap_active_[i]) {
            tap_active_[i] = false;
            transport_.SendTouch({pid, protocol::TouchAction::Up, profile_.taps[i].norm_x, profile_.taps[i].norm_y, 0.0f});
        }
    }
}

void Keymapper::Tick(uint32_t now_ms) {
    if (look_active_ && profile_.mouse_look.idle_reset_ms > 0) {
        if (now_ms - look_last_move_ms_ >= static_cast<uint32_t>(profile_.mouse_look.idle_reset_ms)) {
            ReleaseMouseLook();
        }
    }
    if (profile_.gamepad.enabled) {
        PollXInput(now_ms);
    }
}

void Keymapper::PollXInput(uint32_t now_ms) {
    if (!xinput_get_state_) return;
    XINPUT_STATE st{};
    if (xinput_get_state_(0, &st) != ERROR_SUCCESS) return;

    auto norm_axis = [](SHORT v) -> float {
        float f = static_cast<float>(v) / 32767.0f;
        if (std::fabs(f) < 0.16f) return 0.0f;
        return std::clamp(f, -1.0f, 1.0f);
    };

    // Left stick -> Virtual Joystick (when WASD isn't active)
    if (profile_.joystick.enabled && !joy_wasd_up_ && !joy_wasd_down_ && !joy_wasd_left_ && !joy_wasd_right_) {
        float lx = norm_axis(st.Gamepad.sThumbLX);
        float ly = -norm_axis(st.Gamepad.sThumbLY);
        if (lx != 0.0f || ly != 0.0f) {
            const float tx = std::clamp(profile_.joystick.center_x + lx * profile_.joystick.radius_x, 0.01f, 0.99f);
            const float ty = std::clamp(profile_.joystick.center_y + ly * profile_.joystick.radius_y, 0.01f, 0.99f);
            if (!gp_joy_active_) {
                gp_joy_active_ = true;
                transport_.SendTouch({kPointerJoystick, protocol::TouchAction::Down, profile_.joystick.center_x, profile_.joystick.center_y, 1.0f});
            }
            transport_.SendTouch({kPointerJoystick, protocol::TouchAction::Move, tx, ty, 1.0f});
        } else if (gp_joy_active_) {
            gp_joy_active_ = false;
            transport_.SendTouch({kPointerJoystick, protocol::TouchAction::Up, profile_.joystick.center_x, profile_.joystick.center_y, 0.0f});
        }
    }

    // Right stick -> Camera Pan
    float rx = norm_axis(st.Gamepad.sThumbRX);
    float ry = -norm_axis(st.Gamepad.sThumbRY);
    if (rx != 0.0f || ry != 0.0f) {
        const float sens = profile_.gamepad.right_stick_sens / std::max(profile_.mouse_look.sens_x, 0.0001f);
        ProcessMouseLookDelta(rx * sens * 0.6f, ry * sens * 0.4f, now_ms);
    }

    const uint16_t btns = st.Gamepad.wButtons;
    auto check_btn = [&](uint16_t mask, int ptr_id, float x, float y) {
        bool was = (prev_gp_buttons_ & mask) != 0;
        bool now = (btns & mask) != 0;
        if (now != was) {
            transport_.SendTouch({ptr_id, now ? protocol::TouchAction::Down : protocol::TouchAction::Up, x, y, now ? 1.0f : 0.0f});
        }
    };

    const auto& gp = profile_.gamepad;
    check_btn(XINPUT_GAMEPAD_A, 11, gp.a_x, gp.a_y);
    check_btn(XINPUT_GAMEPAD_B, 12, gp.b_x, gp.b_y);
    check_btn(XINPUT_GAMEPAD_X, 13, gp.x_x, gp.x_y);
    check_btn(XINPUT_GAMEPAD_Y, 14, gp.y_x, gp.y_y);
    prev_gp_buttons_ = btns;

    bool lt = st.Gamepad.bLeftTrigger > 48;
    if (lt != prev_gp_lt_) {
        prev_gp_lt_ = lt;
        transport_.SendTouch({15, lt ? protocol::TouchAction::Down : protocol::TouchAction::Up, gp.lb_x, gp.lb_y, lt ? 1.0f : 0.0f});
    }
    bool rt = st.Gamepad.bRightTrigger > 48;
    if (rt != prev_gp_rt_) {
        prev_gp_rt_ = rt;
        transport_.SendTouch({10, rt ? protocol::TouchAction::Down : protocol::TouchAction::Up, gp.rb_x, gp.rb_y, rt ? 1.0f : 0.0f});
    }
}

std::string Keymapper::InputCodeName(int code) {
    if (code == kMouseLeftCode) return "LMB";
    if (code == kMouseRightCode) return "RMB";
    if (code == kMouseMiddleCode) return "MMB";
    if (code == kMouseX1Code) return "MX1";
    if (code == kMouseX2Code) return "MX2";
    if (code <= 0) return "NONE";
    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(code));
    return (name && name[0]) ? std::string(name) : ("SC" + std::to_string(code));
}

int Keymapper::SdlScancodeToAndroidKeycode(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        return 29 + (sc - SDL_SCANCODE_A); // KEYCODE_A = 29
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) {
        return 8 + (sc - SDL_SCANCODE_1); // KEYCODE_1 = 8
    }
    if (sc == SDL_SCANCODE_0) return 7; // KEYCODE_0
    switch (sc) {
        case SDL_SCANCODE_RETURN: return 66;
        case SDL_SCANCODE_ESCAPE: return 4;   // BACK
        case SDL_SCANCODE_BACKSPACE: return 67;
        case SDL_SCANCODE_TAB: return 61;
        case SDL_SCANCODE_SPACE: return 62;
        case SDL_SCANCODE_MINUS: return 69;
        case SDL_SCANCODE_EQUALS: return 70;
        case SDL_SCANCODE_LEFTBRACKET: return 71;
        case SDL_SCANCODE_RIGHTBRACKET: return 72;
        case SDL_SCANCODE_BACKSLASH: return 73;
        case SDL_SCANCODE_SEMICOLON: return 74;
        case SDL_SCANCODE_APOSTROPHE: return 75;
        case SDL_SCANCODE_GRAVE: return 68;
        case SDL_SCANCODE_COMMA: return 55;
        case SDL_SCANCODE_PERIOD: return 56;
        case SDL_SCANCODE_SLASH: return 76;
        case SDL_SCANCODE_RIGHT: return 22;
        case SDL_SCANCODE_LEFT: return 21;
        case SDL_SCANCODE_DOWN: return 20;
        case SDL_SCANCODE_UP: return 19;
        case SDL_SCANCODE_LSHIFT: return 59;
        case SDL_SCANCODE_RSHIFT: return 60;
        case SDL_SCANCODE_LCTRL: return 113;
        case SDL_SCANCODE_RCTRL: return 114;
        case SDL_SCANCODE_LALT: return 57;
        case SDL_SCANCODE_RALT: return 58;
        case SDL_SCANCODE_DELETE: return 112;
        case SDL_SCANCODE_HOME: return 3;
        default: return 0;
    }
}

}  // namespace dexlink
