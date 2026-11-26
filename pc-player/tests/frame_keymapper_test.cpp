#include "dexlink_protocol.hpp"
#include "input/Keymapper.hpp"
#include "net/UsbTransport.hpp"
#include "video/D3D11Renderer.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

extern "C" {
#include <libavutil/frame.h>
}

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::cerr << "CHECK FAILED at line " << __LINE__ << ": " #cond << std::endl; \
            return 1;                                                                    \
        }                                                                                \
    } while (0)

namespace {

bool RecvExact(SOCKET s, uint8_t* dst, size_t len) {
    size_t got = 0;
    while (got < len) {
        int n = recv(s, reinterpret_cast<char*>(dst + got), static_cast<int>(len - got), 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

int main() {
    std::cout << "[1/3] FrameSlot drop-oldest check..." << std::endl;
    {
        dexlink::FrameSlot slot;
        AVFrame* f1 = av_frame_alloc();
        AVFrame* f2 = av_frame_alloc();
        AVFrame* f3 = av_frame_alloc();
        f1->pts = 100;
        f2->pts = 200;
        f3->pts = 300;

        slot.PushDropOldest(f1);
        slot.PushDropOldest(f2);
        slot.PushDropOldest(f3);

        CHECK(slot.DroppedCount() == 2);
        AVFrame* got = slot.PopLatest();
        CHECK(got != nullptr && got->pts == 300);
        av_frame_free(&got);
        CHECK(slot.PopLatest() == nullptr);
    }

    std::cout << "[2/3] Keymapper JSON profile round-trip..." << std::endl;
    {
        dexlink::SharedTelemetry tel;
        dexlink::UsbTransport transport(tel);
        dexlink::Keymapper km(transport);

        const std::filesystem::path tmp_dir = std::filesystem::temp_directory_path() / "dexlink_km_test";
        std::filesystem::create_directories(tmp_dir);
        km.SetProfileDirectory(tmp_dir.string());

        km.Profile().name = "TestGame";
        km.Profile().package_name = "com.example.game";
        km.Profile().joystick.center_x = 0.2f;
        km.Profile().joystick.center_y = 0.8f;
        CHECK(km.SaveProfile("test_game.json"));

        km.Profile().name = "Mutated";
        CHECK(km.LoadProfile("test_game.json"));
        CHECK(km.Profile().name == "TestGame");
        CHECK(km.Profile().package_name == "com.example.game");
        CHECK(std::fabs(km.Profile().joystick.center_x - 0.2f) < 1e-5f);
        CHECK(dexlink::Keymapper::SdlScancodeToAndroidKeycode(SDL_SCANCODE_W) == 51);
        CHECK(dexlink::Keymapper::SdlScancodeToAndroidKeycode(SDL_SCANCODE_SPACE) == 62);

        std::filesystem::remove_all(tmp_dir);
    }

    std::cout << "[3/3] UsbTransport loopback + WASD + MouseLook..." << std::endl;
    {
        dexlink::SharedTelemetry tel;
        dexlink::UsbTransport transport(tel);
        CHECK(transport.Start(nullptr, nullptr));

        SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        CHECK(client != INVALID_SOCKET);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(dexlink::protocol::kVideoControlPort);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        CHECK(connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

        DWORD timeout_ms = 500;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

        for (int wait = 0; wait < 40 && !tel.Read().video_connected; ++wait) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(tel.Read().video_connected);

        dexlink::Keymapper km(transport);
        km.SetGamingMode(true);

        dexlink::VideoRect vr{0.0f, 0.0f, 1920.0f, 1080.0f};
        SDL_Event w_down{};
        w_down.type = SDL_KEYDOWN;
        w_down.key.keysym.scancode = SDL_SCANCODE_W;
        km.HandleSdlEvent(w_down, vr, false, false);

        std::vector<dexlink::protocol::TouchEventPayload> touches;
        uint8_t hdr_buf[dexlink::protocol::kHeaderSize];
        while (touches.size() < 2) {
            CHECK(RecvExact(client, hdr_buf, sizeof(hdr_buf)));
            dexlink::protocol::PacketHeader hdr{};
            CHECK(dexlink::protocol::DecodeHeader(hdr_buf, hdr));
            std::vector<uint8_t> payload(hdr.payload_size);
            if (hdr.payload_size > 0) CHECK(RecvExact(client, payload.data(), payload.size()));
            if (hdr.type != dexlink::protocol::PacketType::TouchEvent) continue;
            dexlink::protocol::TouchEventPayload tev{};
            CHECK(dexlink::protocol::DecodeTouch(payload, tev));
            touches.push_back(tev);
        }
        CHECK(touches[0].pointer_id == 1 && touches[0].action == dexlink::protocol::TouchAction::Down);
        CHECK(touches[1].pointer_id == 1 && touches[1].action == dexlink::protocol::TouchAction::Move);
        CHECK(touches[1].norm_y < touches[0].norm_y);

        SDL_Event look_ev{};
        look_ev.type = SDL_MOUSEMOTION;
        look_ev.motion.xrel = 500;
        look_ev.motion.yrel = 0;
        km.HandleSdlEvent(look_ev, vr, false, false);

        std::vector<dexlink::protocol::TouchEventPayload> look_touches;
        while (look_touches.size() < 5) {
            CHECK(RecvExact(client, hdr_buf, sizeof(hdr_buf)));
            dexlink::protocol::PacketHeader hdr{};
            CHECK(dexlink::protocol::DecodeHeader(hdr_buf, hdr));
            std::vector<uint8_t> payload(hdr.payload_size);
            if (hdr.payload_size > 0) CHECK(RecvExact(client, payload.data(), payload.size()));
            if (hdr.type != dexlink::protocol::PacketType::TouchEvent) continue;
            dexlink::protocol::TouchEventPayload tev{};
            CHECK(dexlink::protocol::DecodeTouch(payload, tev));
            look_touches.push_back(tev);
        }
        CHECK(look_touches[0].pointer_id == 2 && look_touches[0].action == dexlink::protocol::TouchAction::Down);
        CHECK(look_touches[1].pointer_id == 2 && look_touches[1].action == dexlink::protocol::TouchAction::Move);
        CHECK(look_touches[2].pointer_id == 2 && look_touches[2].action == dexlink::protocol::TouchAction::Up);
        CHECK(look_touches[3].pointer_id == 2 && look_touches[3].action == dexlink::protocol::TouchAction::Down);
        CHECK(look_touches[4].pointer_id == 2 && look_touches[4].action == dexlink::protocol::TouchAction::Move);

        closesocket(client);
        km.ReleaseAll();
        transport.Stop();
    }

    std::cout << "dexlink_core_test: ALL PASSED" << std::endl;
    return 0;
}
