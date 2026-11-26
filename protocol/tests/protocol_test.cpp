#include "../cpp/dexlink_protocol.hpp"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

using namespace dexlink::protocol;

int main() {
    static_assert(kHeaderSize == 12, "Header must be exactly 12 bytes");
    static_assert(TouchEventPayload::kSize == 20, "Touch payload must be 20 bytes");
    static_assert(KeyEventPayload::kSize == 12, "Key payload must be 12 bytes");
    static_assert(ControlCommandPayload::kSize == 12, "Control payload must be 12 bytes");
    static_assert(TelemetryPayload::kSize == 12, "Telemetry payload must be 12 bytes");

    PacketHeader hdr{
        .type = PacketType::VideoNal,
        .flags = kVideoFlagConfig | kVideoFlagKeyframe,
        .reserved = 0x1234,
        .payload_size = 65536,
        .timestamp_ms = 0xABCDEF01u,
    };
    auto hdr_bytes = EncodeHeader(hdr);
    assert(hdr_bytes[0] == 0x01);
    assert(hdr_bytes[1] == 0x03);
    assert(hdr_bytes[2] == 0x34 && hdr_bytes[3] == 0x12);
    assert(hdr_bytes[4] == 0x00 && hdr_bytes[5] == 0x00 && hdr_bytes[6] == 0x01 && hdr_bytes[7] == 0x00);

    PacketHeader decoded_hdr{};
    assert(DecodeHeader(hdr_bytes, decoded_hdr));
    assert(decoded_hdr.type == hdr.type);
    assert(decoded_hdr.flags == hdr.flags);
    assert(decoded_hdr.reserved == hdr.reserved);
    assert(decoded_hdr.payload_size == hdr.payload_size);
    assert(decoded_hdr.timestamp_ms == hdr.timestamp_ms);

    TouchEventPayload touch{
        .pointer_id = 3,
        .action = TouchAction::Move,
        .norm_x = 0.75f,
        .norm_y = 0.25f,
        .pressure = 0.9f,
    };
    auto touch_bytes = EncodeTouch(touch);
    TouchEventPayload touch_dec{};
    assert(DecodeTouch(touch_bytes, touch_dec));
    assert(touch_dec.pointer_id == 3);
    assert(touch_dec.action == TouchAction::Move);
    assert(std::fabs(touch_dec.norm_x - 0.75f) < 1e-6f);
    assert(std::fabs(touch_dec.norm_y - 0.25f) < 1e-6f);
    assert(std::fabs(touch_dec.pressure - 0.9f) < 1e-6f);

    KeyEventPayload key{.keycode = 51, .action = KeyAction::Down, .meta_state = 1};
    KeyEventPayload key_dec{};
    assert(DecodeKey(EncodeKey(key), key_dec));
    assert(key_dec.keycode == 51 && key_dec.action == KeyAction::Down && key_dec.meta_state == 1);

    ControlCommandPayload ctrl{
        .cmd = ControlCmd::SetRes,
        .arg_u8 = 0,
        .arg_u16 = 120,
        .arg0 = 1920,
        .arg1 = 1080,
    };
    ControlCommandPayload ctrl_dec{};
    assert(DecodeControl(EncodeControl(ctrl), ctrl_dec));
    assert(ctrl_dec.cmd == ControlCmd::SetRes && ctrl_dec.arg_u16 == 120 && ctrl_dec.arg0 == 1920 && ctrl_dec.arg1 == 1080);

    TelemetryPayload tel{.temp_c = 41.5f, .enc_fps = 59.94f, .battery_pct = 87, .status_flags = kTelemetryDexMode};
    TelemetryPayload tel_dec{};
    assert(DecodeTelemetry(EncodeTelemetry(tel), tel_dec));
    assert(std::fabs(tel_dec.temp_c - 41.5f) < 1e-5f);
    assert(std::fabs(tel_dec.enc_fps - 59.94f) < 1e-4f);
    assert(tel_dec.battery_pct == 87 && tel_dec.status_flags == kTelemetryDexMode);

    std::array<uint8_t, kHeaderSize> bad_hdr = hdr_bytes;
    bad_hdr[0] = 0x7F;
    assert(!DecodeHeader(bad_hdr, decoded_hdr));

    std::cout << "protocol_test: ALL PASSED\n";
    return 0;
}
