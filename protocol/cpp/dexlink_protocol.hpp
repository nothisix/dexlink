#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace dexlink::protocol {

inline constexpr uint16_t kVideoControlPort = 27183;
inline constexpr uint16_t kAudioTelemetryPort = 27184;
inline constexpr uint32_t kMaxPayloadSize = 16u * 1024u * 1024u;
inline constexpr size_t kHeaderSize = 12;

enum class PacketType : uint8_t {
    VideoNal = 0x01,
    AudioPcm = 0x02,
    TouchEvent = 0x03,
    KeyEvent = 0x04,
    ControlCommand = 0x05,
    Telemetry = 0x06,
};

enum VideoFlags : uint8_t {
    kVideoFlagNone = 0x00,
    kVideoFlagConfig = 0x01,
    kVideoFlagKeyframe = 0x02,
    kVideoFlagHevc = 0x04,
};

enum AudioFlags : uint8_t {
    kAudioFlagPcm48kStereoS16 = 0x01,
};

enum TelemetryFlags : uint8_t {
    kTelemetryCharging = 0x01,
    kTelemetryAudioActive = 0x02,
    kTelemetryScreenOff = 0x04,
    kTelemetryDexMode = 0x08,
    kTelemetryThermalWarn = 0x10,
};

enum class TouchAction : uint8_t {
    Down = 0,
    Up = 1,
    Move = 2,
    Cancel = 3,
    HoverMove = 4,
};

enum class KeyAction : uint8_t {
    Down = 0,
    Up = 1,
};

enum class ControlCmd : uint8_t {
    SetRes = 0x01,
    SetBitrate = 0x02,
    ScreenOff = 0x03,
    RequestIdr = 0x04,
    SetMode = 0x05,
    Ping = 0x06,
    Pong = 0x07,
    LaunchApp = 0x08,
    NavAction = 0x09,
};

enum class DisplayMode : uint8_t {
    Mirror = 0,
    Dex = 1,
};

enum class VideoCodec : uint8_t {
    H264 = 0,
    Hevc = 1,
};

struct PacketHeader {
    PacketType type = PacketType::VideoNal;
    uint8_t flags = 0;
    uint16_t reserved = 0;
    uint32_t payload_size = 0;
    uint32_t timestamp_ms = 0;
};

struct TouchEventPayload {
    int32_t pointer_id = 0;
    TouchAction action = TouchAction::Down;
    float norm_x = 0.0f;
    float norm_y = 0.0f;
    float pressure = 1.0f;

    static constexpr size_t kSize = 20;
};

struct KeyEventPayload {
    int32_t keycode = 0;
    KeyAction action = KeyAction::Down;
    int32_t meta_state = 0;

    static constexpr size_t kSize = 12;
};

struct ControlCommandPayload {
    ControlCmd cmd = ControlCmd::RequestIdr;
    uint8_t arg_u8 = 0;
    uint16_t arg_u16 = 0;
    uint32_t arg0 = 0;
    uint32_t arg1 = 0;

    static constexpr size_t kSize = 12;
};

struct TelemetryPayload {
    float temp_c = 0.0f;
    float enc_fps = 0.0f;
    uint8_t battery_pct = 0;
    uint8_t status_flags = 0;
    uint16_t reserved = 0;

    static constexpr size_t kSize = 12;
};

namespace detail {

inline void WriteU16LE(uint8_t* dst, uint16_t v) noexcept {
    dst[0] = static_cast<uint8_t>(v & 0xFFu);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

inline void WriteU32LE(uint8_t* dst, uint32_t v) noexcept {
    dst[0] = static_cast<uint8_t>(v & 0xFFu);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

inline void WriteI32LE(uint8_t* dst, int32_t v) noexcept {
    uint32_t u = 0;
    std::memcpy(&u, &v, sizeof(u));
    WriteU32LE(dst, u);
}

inline void WriteF32LE(uint8_t* dst, float v) noexcept {
    uint32_t u = 0;
    std::memcpy(&u, &v, sizeof(u));
    WriteU32LE(dst, u);
}

inline uint16_t ReadU16LE(const uint8_t* src) noexcept {
    return static_cast<uint16_t>(src[0]) | (static_cast<uint16_t>(src[1]) << 8);
}

inline uint32_t ReadU32LE(const uint8_t* src) noexcept {
    return static_cast<uint32_t>(src[0]) |
           (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) |
           (static_cast<uint32_t>(src[3]) << 24);
}

inline int32_t ReadI32LE(const uint8_t* src) noexcept {
    uint32_t u = ReadU32LE(src);
    int32_t v = 0;
    std::memcpy(&v, &u, sizeof(v));
    return v;
}

inline float ReadF32LE(const uint8_t* src) noexcept {
    uint32_t u = ReadU32LE(src);
    float v = 0.0f;
    std::memcpy(&v, &u, sizeof(v));
    return v;
}

}  // namespace detail

inline std::array<uint8_t, kHeaderSize> EncodeHeader(const PacketHeader& hdr) noexcept {
    std::array<uint8_t, kHeaderSize> out{};
    out[0] = static_cast<uint8_t>(hdr.type);
    out[1] = hdr.flags;
    detail::WriteU16LE(out.data() + 2, hdr.reserved);
    detail::WriteU32LE(out.data() + 4, hdr.payload_size);
    detail::WriteU32LE(out.data() + 8, hdr.timestamp_ms);
    return out;
}

inline bool DecodeHeader(std::span<const uint8_t> buf, PacketHeader& out) noexcept {
    if (buf.size() < kHeaderSize) {
        return false;
    }
    const uint8_t raw_type = buf[0];
    if (raw_type < 0x01 || raw_type > 0x06) {
        return false;
    }
    out.type = static_cast<PacketType>(raw_type);
    out.flags = buf[1];
    out.reserved = detail::ReadU16LE(buf.data() + 2);
    out.payload_size = detail::ReadU32LE(buf.data() + 4);
    out.timestamp_ms = detail::ReadU32LE(buf.data() + 8);
    return out.payload_size <= kMaxPayloadSize;
}

inline std::array<uint8_t, TouchEventPayload::kSize> EncodeTouch(const TouchEventPayload& ev) noexcept {
    std::array<uint8_t, TouchEventPayload::kSize> out{};
    detail::WriteI32LE(out.data(), ev.pointer_id);
    out[4] = static_cast<uint8_t>(ev.action);
    out[5] = 0;
    out[6] = 0;
    out[7] = 0;
    detail::WriteF32LE(out.data() + 8, ev.norm_x);
    detail::WriteF32LE(out.data() + 12, ev.norm_y);
    detail::WriteF32LE(out.data() + 16, ev.pressure);
    return out;
}

inline bool DecodeTouch(std::span<const uint8_t> buf, TouchEventPayload& out) noexcept {
    if (buf.size() < TouchEventPayload::kSize) {
        return false;
    }
    out.pointer_id = detail::ReadI32LE(buf.data());
    out.action = static_cast<TouchAction>(buf[4]);
    out.norm_x = detail::ReadF32LE(buf.data() + 8);
    out.norm_y = detail::ReadF32LE(buf.data() + 12);
    out.pressure = detail::ReadF32LE(buf.data() + 16);
    return true;
}

inline std::array<uint8_t, KeyEventPayload::kSize> EncodeKey(const KeyEventPayload& ev) noexcept {
    std::array<uint8_t, KeyEventPayload::kSize> out{};
    detail::WriteI32LE(out.data(), ev.keycode);
    out[4] = static_cast<uint8_t>(ev.action);
    out[5] = 0;
    out[6] = 0;
    out[7] = 0;
    detail::WriteI32LE(out.data() + 8, ev.meta_state);
    return out;
}

inline bool DecodeKey(std::span<const uint8_t> buf, KeyEventPayload& out) noexcept {
    if (buf.size() < KeyEventPayload::kSize) {
        return false;
    }
    out.keycode = detail::ReadI32LE(buf.data());
    out.action = static_cast<KeyAction>(buf[4]);
    out.meta_state = detail::ReadI32LE(buf.data() + 8);
    return true;
}

inline std::array<uint8_t, ControlCommandPayload::kSize> EncodeControl(const ControlCommandPayload& cmd) noexcept {
    std::array<uint8_t, ControlCommandPayload::kSize> out{};
    out[0] = static_cast<uint8_t>(cmd.cmd);
    out[1] = cmd.arg_u8;
    detail::WriteU16LE(out.data() + 2, cmd.arg_u16);
    detail::WriteU32LE(out.data() + 4, cmd.arg0);
    detail::WriteU32LE(out.data() + 8, cmd.arg1);
    return out;
}

inline bool DecodeControl(std::span<const uint8_t> buf, ControlCommandPayload& out) noexcept {
    if (buf.size() < ControlCommandPayload::kSize) {
        return false;
    }
    out.cmd = static_cast<ControlCmd>(buf[0]);
    out.arg_u8 = buf[1];
    out.arg_u16 = detail::ReadU16LE(buf.data() + 2);
    out.arg0 = detail::ReadU32LE(buf.data() + 4);
    out.arg1 = detail::ReadU32LE(buf.data() + 8);
    return true;
}

inline std::array<uint8_t, TelemetryPayload::kSize> EncodeTelemetry(const TelemetryPayload& t) noexcept {
    std::array<uint8_t, TelemetryPayload::kSize> out{};
    detail::WriteF32LE(out.data(), t.temp_c);
    detail::WriteF32LE(out.data() + 4, t.enc_fps);
    out[8] = t.battery_pct;
    out[9] = t.status_flags;
    detail::WriteU16LE(out.data() + 10, t.reserved);
    return out;
}

inline bool DecodeTelemetry(std::span<const uint8_t> buf, TelemetryPayload& out) noexcept {
    if (buf.size() < TelemetryPayload::kSize) {
        return false;
    }
    out.temp_c = detail::ReadF32LE(buf.data());
    out.enc_fps = detail::ReadF32LE(buf.data() + 4);
    out.battery_pct = buf[8];
    out.status_flags = buf[9];
    out.reserved = detail::ReadU16LE(buf.data() + 10);
    return true;
}

inline std::vector<uint8_t> BuildPacket(
    PacketType type,
    uint8_t flags,
    uint32_t timestamp_ms,
    std::span<const uint8_t> payload,
    uint16_t reserved = 0
) {
    PacketHeader hdr{
        .type = type,
        .flags = flags,
        .reserved = reserved,
        .payload_size = static_cast<uint32_t>(payload.size()),
        .timestamp_ms = timestamp_ms,
    };
    const auto hdr_bytes = EncodeHeader(hdr);
    std::vector<uint8_t> packet(kHeaderSize + payload.size());
    std::memcpy(packet.data(), hdr_bytes.data(), kHeaderSize);
    if (!payload.empty()) {
        std::memcpy(packet.data() + kHeaderSize, payload.data(), payload.size());
    }
    return packet;
}

}  // namespace dexlink::protocol
