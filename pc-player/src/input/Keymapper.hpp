#pragma once

#include "core/Config.hpp"
#include "dexlink_protocol.hpp"
#include "net/UsbTransport.hpp"

#include <SDL2/SDL.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dexlink {

struct VideoRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 1.0f;
    float h = 1.0f;
};

struct JoystickBinding {
    bool enabled = true;
    float center_x = 0.16f;
    float center_y = 0.72f;
    float radius_x = 0.08f;
    float radius_y = 0.14f;
    int key_up = SDL_SCANCODE_W;
    int key_down = SDL_SCANCODE_S;
    int key_left = SDL_SCANCODE_A;
    int key_right = SDL_SCANCODE_D;
};

struct MouseLookBinding {
    bool enabled = true;
    float center_x = 0.65f;
    float center_y = 0.50f;
    float sens_x = 0.0014f;
    float sens_y = 0.0022f;
    float bound_half_w = 0.20f;
    float bound_half_h = 0.25f;
    int idle_reset_ms = 220;
};

// Negative input_code values encode mouse buttons (-1=LMB, -2=RMB, -3=MMB, -4=X1, -5=X2)
// Positive input_code values encode SDL_Scancode.
struct TapBinding {
    std::string label;
    int input_code = 0;
    float norm_x = 0.5f;
    float norm_y = 0.5f;
};

struct GamepadBinding {
    bool enabled = true;
    float right_stick_sens = 0.015f;
    float a_x = 0.88f, a_y = 0.78f; // Jump
    float b_x = 0.88f, b_y = 0.88f; // Crouch
    float x_x = 0.78f, x_y = 0.82f; // Reload
    float y_x = 0.50f, y_y = 0.88f; // Weapon swap
    float rb_x = 0.85f, rb_y = 0.60f; // Fire
    float lb_x = 0.15f, lb_y = 0.45f; // Scope
};

struct KeymapProfile {
    std::string name = "default";
    std::string package_name;
    JoystickBinding joystick{};
    MouseLookBinding mouse_look{};
    GamepadBinding gamepad{};
    std::vector<TapBinding> taps;
};

class Keymapper {
public:
    explicit Keymapper(UsbTransport& transport);
    ~Keymapper();

    void SetProfileDirectory(const std::string& dir);
    std::vector<std::string> ListProfiles() const;
    bool LoadProfile(const std::string& filename);
    bool SaveProfile(const std::string& filename) const;

    KeymapProfile& Profile() { return profile_; }
    const KeymapProfile& Profile() const { return profile_; }

    void SetGamingMode(bool enabled);
    bool GamingMode() const { return gaming_mode_; }

    // Handles SDL input events when not captured by UI widgets.
    void HandleSdlEvent(const SDL_Event& ev, const VideoRect& vrect, bool ui_wants_mouse, bool ui_wants_kbd);

    // Per-frame update (mouse look idle reset + XInput polling)
    void Tick(uint32_t now_ms);

    void ReleaseAll(bool direct_only = false);

    static std::string InputCodeName(int code);
    static int SdlScancodeToAndroidKeycode(SDL_Scancode sc);

private:
    void UpdateJoystickTouch();
    void ProcessMouseLookDelta(float dx, float dy, uint32_t now_ms);
    void ReleaseMouseLook();
    void TriggerTap(int input_code, bool down);
    void PollXInput(uint32_t now_ms);

    bool ScreenToNorm(float sx, float sy, const VideoRect& vr, float& nx, float& ny) const;

    UsbTransport& transport_;
    std::string profile_dir_;
    KeymapProfile profile_{};
    bool gaming_mode_ = false;

    // Direct touch state (pointer 0)
    bool direct_mouse_down_ = false;

    // Virtual joystick state (pointer 1)
    bool joy_wasd_up_ = false;
    bool joy_wasd_down_ = false;
    bool joy_wasd_left_ = false;
    bool joy_wasd_right_ = false;
    bool joy_active_ = false;
    float joy_x_ = 0.0f;
    float joy_y_ = 0.0f;

    // Mouse look state (pointer 2)
    bool look_active_ = false;
    float look_cur_x_ = 0.65f;
    float look_cur_y_ = 0.50f;
    uint32_t look_last_move_ms_ = 0;

    // Tap active states (pointer 3..15)
    static constexpr int kTapPointerBase = 3;
    static constexpr int kMaxTapPointers = 13;
    std::array<bool, kMaxTapPointers> tap_active_{};

    // XInput dynamic loader
    using XInputGetStateFn = unsigned long(__stdcall*)(unsigned long, void*);
    HMODULE xinput_dll_ = nullptr;
    XInputGetStateFn xinput_get_state_ = nullptr;
    uint16_t prev_gp_buttons_ = 0;
    bool prev_gp_lt_ = false;
    bool prev_gp_rt_ = false;
    bool gp_joy_active_ = false;
};

}  // namespace dexlink
