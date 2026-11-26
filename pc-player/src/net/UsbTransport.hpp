#pragma once

#include "core/Config.hpp"
#include "dexlink_protocol.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace dexlink {

class UsbTransport {
public:
    using VideoCallback = std::function<void(uint8_t flags, uint32_t ts_ms, const uint8_t* data, size_t len)>;
    using AudioCallback = std::function<void(const uint8_t* pcm_data, size_t len)>;

    explicit UsbTransport(SharedTelemetry& telemetry);
    ~UsbTransport();

    UsbTransport(const UsbTransport&) = delete;
    UsbTransport& operator=(const UsbTransport&) = delete;

    bool Start(VideoCallback on_video, AudioCallback on_audio);
    void Stop();

    void SendTouch(const protocol::TouchEventPayload& ev);
    void SendKey(const protocol::KeyEventPayload& ev);
    void SendControl(const protocol::ControlCommandPayload& cmd, const std::string& extra_str = {});

private:
    void VideoAcceptLoop();
    void AudioAcceptLoop();
    void PingLoop();

    void HandleVideoClient(SOCKET sock);
    void HandleAudioClient(SOCKET sock);

    bool SendRawPacket(
        protocol::PacketType type,
        uint8_t flags,
        uint32_t ts_ms,
        const uint8_t* payload,
        size_t payload_len
    );

    static SOCKET CreateListener(uint16_t port);
    static void ConfigureNoDelay(SOCKET sock);
    static bool RecvExact(SOCKET sock, uint8_t* dst, size_t len);
    static bool SendAll(SOCKET sock, const uint8_t* src, size_t len);
    static uint32_t MonotonicMs();

    SharedTelemetry& telemetry_;
    VideoCallback on_video_;
    AudioCallback on_audio_;

    std::atomic<bool> running_{false};
    SOCKET video_listen_ = INVALID_SOCKET;
    SOCKET audio_listen_ = INVALID_SOCKET;

    std::mutex video_send_mu_;
    SOCKET video_client_ = INVALID_SOCKET;
    SOCKET audio_client_ = INVALID_SOCKET;

    std::thread video_thread_;
    std::thread audio_thread_;
    std::thread ping_thread_;

    std::atomic<uint32_t> last_ping_seq_{0};
    std::atomic<uint32_t> last_ping_sent_ms_{0};
};

}  // namespace dexlink
