#pragma once

#include "dexlink_protocol.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace dexlink {

struct AppConfig {
    int width = 1920;
    int height = 1080;
    int fps = 60;
    int bitrate_mbps = 20;
    int dpi = 320;
    protocol::DisplayMode mode = protocol::DisplayMode::Dex;
    protocol::VideoCodec codec = protocol::VideoCodec::H264;
    bool screen_off = false;
    bool fullscreen = false;
    bool drawer_open = false;
    bool keymap_edit = false;
    bool gaming_mode = false;
    bool vsync = true;
    std::string active_profile = "default.json";
    std::string launch_package;
    std::string adb_path = "adb";
};

struct TelemetrySnapshot {
    bool usb_attached = false;
    bool video_connected = false;
    bool audio_connected = false;
    bool screen_off = false;
    bool dex_mode = true;
    bool thermal_warn = false;
    std::string device_serial = "NONE";
    std::string device_state = "WAIT";
    float temp_c = 0.0f;
    int battery_pct = 0;
    float enc_fps = 0.0f;
    float dec_fps = 0.0f;
    float render_fps = 0.0f;
    uint32_t rtt_ms = 0;
    uint64_t dropped_frames = 0;
    float rx_mbps = 0.0f;
    int video_width = 1920;
    int video_height = 1080;
    std::string last_error;
};

class SharedTelemetry {
public:
    void Update(auto&& fn) {
        std::lock_guard<std::mutex> lock(mu_);
        fn(state_);
    }

    TelemetrySnapshot Read() const {
        std::lock_guard<std::mutex> lock(mu_);
        return state_;
    }

private:
    mutable std::mutex mu_;
    TelemetrySnapshot state_{};
};

}  // namespace dexlink
