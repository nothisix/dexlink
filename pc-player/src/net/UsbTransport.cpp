#include "net/UsbTransport.hpp"

#include <chrono>
#include <cstring>

namespace dexlink {

UsbTransport::UsbTransport(SharedTelemetry& telemetry) : telemetry_(telemetry) {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

UsbTransport::~UsbTransport() {
    Stop();
    WSACleanup();
}

uint32_t UsbTransport::MonotonicMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void UsbTransport::ConfigureNoDelay(SOCKET sock) {
    SetHandleInformation(reinterpret_cast<HANDLE>(sock), HANDLE_FLAG_INHERIT, 0);
    BOOL one = TRUE;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    int rcv_buf = 512 * 1024;
    int snd_buf = 128 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcv_buf), sizeof(rcv_buf));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&snd_buf), sizeof(snd_buf));
}

SOCKET UsbTransport::CreateListener(uint16_t port) {
    SOCKET s = WSASocketA(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP,
        nullptr,
        0,
        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT
    );
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);

    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(s, 4) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

bool UsbTransport::RecvExact(SOCKET sock, uint8_t* dst, size_t len) {
    size_t got = 0;
    while (got < len) {
        int n = recv(sock, reinterpret_cast<char*>(dst + got), static_cast<int>(len - got), 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

bool UsbTransport::SendAll(SOCKET sock, const uint8_t* src, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(sock, reinterpret_cast<const char*>(src + sent), static_cast<int>(len - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool UsbTransport::Start(VideoCallback on_video, AudioCallback on_audio) {
    if (running_.exchange(true)) return true;

    on_video_ = std::move(on_video);
    on_audio_ = std::move(on_audio);

    video_listen_ = CreateListener(protocol::kVideoControlPort);
    audio_listen_ = CreateListener(protocol::kAudioTelemetryPort);
    if (video_listen_ == INVALID_SOCKET || audio_listen_ == INVALID_SOCKET) {
        Stop();
        return false;
    }

    video_thread_ = std::thread(&UsbTransport::VideoAcceptLoop, this);
    audio_thread_ = std::thread(&UsbTransport::AudioAcceptLoop, this);
    ping_thread_ = std::thread(&UsbTransport::PingLoop, this);
    return true;
}

void UsbTransport::Stop() {
    if (!running_.exchange(false)) return;

    auto close_sock = [](SOCKET& s) {
        if (s != INVALID_SOCKET) {
            shutdown(s, SD_BOTH);
            closesocket(s);
            s = INVALID_SOCKET;
        }
    };

    close_sock(video_listen_);
    close_sock(audio_listen_);
    {
        std::lock_guard<std::mutex> lock(video_send_mu_);
        close_sock(video_client_);
    }
    close_sock(audio_client_);

    if (video_thread_.joinable()) video_thread_.join();
    if (audio_thread_.joinable()) audio_thread_.join();
    if (ping_thread_.joinable()) ping_thread_.join();
}

void UsbTransport::VideoAcceptLoop() {
    while (running_.load()) {
        SOCKET client = accept(video_listen_, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (!running_.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        ConfigureNoDelay(client);
        {
            std::lock_guard<std::mutex> lock(video_send_mu_);
            if (video_client_ != INVALID_SOCKET) {
                closesocket(video_client_);
            }
            video_client_ = client;
        }
        telemetry_.Update([](TelemetrySnapshot& s) {
            s.video_connected = true;
            s.device_state = "CONNECTED";
        });
        HandleVideoClient(client);
        {
            std::lock_guard<std::mutex> lock(video_send_mu_);
            if (video_client_ == client) {
                closesocket(client);
                video_client_ = INVALID_SOCKET;
            }
        }
        telemetry_.Update([](TelemetrySnapshot& s) {
            s.video_connected = false;
            s.rx_mbps = 0.0f;
            if (s.usb_attached) s.device_state = "WAIT";
        });
    }
}

void UsbTransport::AudioAcceptLoop() {
    while (running_.load()) {
        SOCKET client = accept(audio_listen_, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (!running_.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        ConfigureNoDelay(client);
        audio_client_ = client;
        telemetry_.Update([](TelemetrySnapshot& s) { s.audio_connected = true; });
        HandleAudioClient(client);
        if (audio_client_ == client) {
            closesocket(client);
            audio_client_ = INVALID_SOCKET;
        }
        telemetry_.Update([](TelemetrySnapshot& s) { s.audio_connected = false; });
    }
}

void UsbTransport::HandleVideoClient(SOCKET sock) {
    std::vector<uint8_t> hdr_buf(protocol::kHeaderSize);
    std::vector<uint8_t> payload;

    uint64_t bytes_window = 0;
    auto window_start = std::chrono::steady_clock::now();

    while (running_.load()) {
        if (!RecvExact(sock, hdr_buf.data(), protocol::kHeaderSize)) break;
        protocol::PacketHeader hdr{};
        if (!protocol::DecodeHeader(hdr_buf, hdr)) break;

        payload.resize(hdr.payload_size);
        if (hdr.payload_size > 0 && !RecvExact(sock, payload.data(), hdr.payload_size)) {
            break;
        }

        bytes_window += protocol::kHeaderSize + hdr.payload_size;
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - window_start).count();
        if (elapsed_ms >= 500) {
            float mbps = (static_cast<float>(bytes_window) * 8.0f) / (static_cast<float>(elapsed_ms) * 1000.0f);
            telemetry_.Update([mbps](TelemetrySnapshot& s) { s.rx_mbps = mbps; });
            bytes_window = 0;
            window_start = now;
        }

        switch (hdr.type) {
            case protocol::PacketType::VideoNal:
                if (on_video_ && !payload.empty()) {
                    on_video_(hdr.flags, hdr.timestamp_ms, payload.data(), payload.size());
                }
                break;

            case protocol::PacketType::Telemetry: {
                protocol::TelemetryPayload tel{};
                if (protocol::DecodeTelemetry(payload, tel)) {
                    telemetry_.Update([&](TelemetrySnapshot& s) {
                        if (tel.temp_c > 0.1f) s.temp_c = tel.temp_c;
                        s.battery_pct = tel.battery_pct;
                        s.enc_fps = tel.enc_fps;
                        s.screen_off = (tel.status_flags & protocol::kTelemetryScreenOff) != 0;
                        s.dex_mode = (tel.status_flags & protocol::kTelemetryDexMode) != 0;
                        s.thermal_warn = (tel.status_flags & protocol::kTelemetryThermalWarn) != 0;
                    });
                }
                break;
            }

            case protocol::PacketType::ControlCommand: {
                protocol::ControlCommandPayload cmd{};
                if (protocol::DecodeControl(payload, cmd) && cmd.cmd == protocol::ControlCmd::Pong) {
                    if (cmd.arg0 == last_ping_seq_.load()) {
                        uint32_t rtt = MonotonicMs() - last_ping_sent_ms_.load();
                        telemetry_.Update([rtt](TelemetrySnapshot& s) { s.rtt_ms = rtt; });
                    }
                }
                break;
            }

            default:
                break;
        }
    }
}

void UsbTransport::HandleAudioClient(SOCKET sock) {
    std::vector<uint8_t> hdr_buf(protocol::kHeaderSize);
    std::vector<uint8_t> payload;

    while (running_.load()) {
        if (!RecvExact(sock, hdr_buf.data(), protocol::kHeaderSize)) break;
        protocol::PacketHeader hdr{};
        if (!protocol::DecodeHeader(hdr_buf, hdr)) break;

        payload.resize(hdr.payload_size);
        if (hdr.payload_size > 0 && !RecvExact(sock, payload.data(), hdr.payload_size)) {
            break;
        }

        if (hdr.type == protocol::PacketType::AudioPcm && on_audio_ && !payload.empty()) {
            on_audio_(payload.data(), payload.size());
        } else if (hdr.type == protocol::PacketType::Telemetry) {
            protocol::TelemetryPayload tel{};
            if (protocol::DecodeTelemetry(payload, tel)) {
                telemetry_.Update([&](TelemetrySnapshot& s) {
                    if (tel.temp_c > 0.1f) s.temp_c = tel.temp_c;
                    s.battery_pct = tel.battery_pct;
                    s.thermal_warn = (tel.status_flags & protocol::kTelemetryThermalWarn) != 0;
                });
            }
        }
    }
}

void UsbTransport::PingLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!running_.load()) break;
        uint32_t seq = last_ping_seq_.fetch_add(1) + 1;
        last_ping_sent_ms_.store(MonotonicMs());
        protocol::ControlCommandPayload ping{
            .cmd = protocol::ControlCmd::Ping,
            .arg_u8 = 0,
            .arg_u16 = 0,
            .arg0 = seq,
            .arg1 = 0,
        };
        SendControl(ping);
    }
}

bool UsbTransport::SendRawPacket(
    protocol::PacketType type,
    uint8_t flags,
    uint32_t ts_ms,
    const uint8_t* payload,
    size_t payload_len
) {
    protocol::PacketHeader hdr{
        .type = type,
        .flags = flags,
        .reserved = 0,
        .payload_size = static_cast<uint32_t>(payload_len),
        .timestamp_ms = ts_ms,
    };
    const auto hdr_bytes = protocol::EncodeHeader(hdr);

    std::lock_guard<std::mutex> lock(video_send_mu_);
    if (video_client_ == INVALID_SOCKET) return false;
    if (!SendAll(video_client_, hdr_bytes.data(), hdr_bytes.size())) return false;
    if (payload_len > 0 && !SendAll(video_client_, payload, payload_len)) return false;
    return true;
}

void UsbTransport::SendTouch(const protocol::TouchEventPayload& ev) {
    const auto bytes = protocol::EncodeTouch(ev);
    SendRawPacket(protocol::PacketType::TouchEvent, 0, MonotonicMs(), bytes.data(), bytes.size());
}

void UsbTransport::SendKey(const protocol::KeyEventPayload& ev) {
    const auto bytes = protocol::EncodeKey(ev);
    SendRawPacket(protocol::PacketType::KeyEvent, 0, MonotonicMs(), bytes.data(), bytes.size());
}

void UsbTransport::SendControl(const protocol::ControlCommandPayload& cmd, const std::string& extra_str) {
    const auto base = protocol::EncodeControl(cmd);
    if (extra_str.empty()) {
        SendRawPacket(protocol::PacketType::ControlCommand, 0, MonotonicMs(), base.data(), base.size());
        return;
    }
    std::vector<uint8_t> combined(base.size() + extra_str.size());
    std::memcpy(combined.data(), base.data(), base.size());
    std::memcpy(combined.data() + base.size(), extra_str.data(), extra_str.size());
    SendRawPacket(protocol::PacketType::ControlCommand, 0, MonotonicMs(), combined.data(), combined.size());
}

}  // namespace dexlink
