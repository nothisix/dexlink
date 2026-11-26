#include "adb/AdbManager.hpp"
#include "audio/WasapiAudioPlayer.hpp"
#include "core/Config.hpp"
#include "input/Keymapper.hpp"
#include "net/UsbTransport.hpp"
#include "ui/DexlinkUi.hpp"
#include "video/D3D11Renderer.hpp"
#include "video/VideoDecoder.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <dwmapi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_sdl2.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string FindProfilesDirectory() {
    char exe_buf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe_buf, MAX_PATH);
    fs::path exe_dir = fs::path(exe_buf).parent_path();

    const fs::path candidates[] = {
        exe_dir / "profiles",
        exe_dir / ".." / "profiles",
        exe_dir / ".." / ".." / "profiles",
        fs::current_path() / "profiles",
        fs::current_path() / ".." / "profiles",
    };
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) {
            return fs::weakly_canonical(c, ec).string();
        }
    }
    return (exe_dir / "profiles").string();
}

void ApplyWindowsDarkFrame(SDL_Window* window) {
    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
    if (SDL_GetWindowWMInfo(window, &wm)) {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(wm.info.win.window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    dexlink::AppConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--mirror") cfg.mode = dexlink::protocol::DisplayMode::Mirror;
        else if (arg == "--dex") cfg.mode = dexlink::protocol::DisplayMode::Dex;
        else if (arg == "--hevc") cfg.codec = dexlink::protocol::VideoCodec::Hevc;
        else if (arg == "--120") cfg.fps = 120;
        else if (arg == "--pkg" && i + 1 < argc) cfg.launch_package = argv[++i];
        else if (arg == "--profile" && i + 1 < argc) cfg.active_profile = argv[++i];
    }

    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        return 1;
    }

    const int init_w = 1280;
    const int init_h = 748; // 720p canvas + 28px telemetry bar
    SDL_Window* window = SDL_CreateWindow(
        "DEXLINK",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        init_w,
        init_h,
        SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!window) {
        SDL_Quit();
        return 1;
    }
    ApplyWindowsDarkFrame(window);

    dexlink::SharedTelemetry telemetry;
    dexlink::FrameSlot frame_slot;
    dexlink::D3D11Renderer renderer(telemetry);
    if (!renderer.Initialize(window, init_w, init_h)) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    dexlink::VideoDecoder decoder(telemetry, frame_slot);
    if (!decoder.Start(renderer.Device(), renderer.Context())) {
        renderer.Shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    dexlink::WasapiAudioPlayer audio_player;
    audio_player.Start();

    dexlink::UsbTransport transport(telemetry);
    transport.Start(
        [&](uint8_t flags, uint32_t ts_ms, const uint8_t* data, size_t len) {
            decoder.PushNalPacket(flags, ts_ms, data, len);
        },
        [&](const uint8_t* pcm, size_t len) {
            audio_player.PushPcm(pcm, len);
        }
    );

    dexlink::AdbManager adb(telemetry);
    adb.Start(cfg);

    dexlink::Keymapper keymapper(transport);
    keymapper.SetProfileDirectory(FindProfilesDirectory());
    keymapper.LoadProfile(cfg.active_profile);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    dexlink::DexlinkUi ui(cfg, telemetry, adb, transport, keymapper);
    ui.InitFontsAndStyle();

    ImGui_ImplSDL2_InitForD3D(window);
    ImGui_ImplDX11_Init(renderer.Device(), renderer.Context());

    bool running = true;
    dexlink::VideoRect video_rect{};
    int render_frames = 0;
    auto fps_window = std::chrono::steady_clock::now();

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);

            if (ev.type == SDL_QUIT) {
                running = false;
                break;
            }
            if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                renderer.Resize(ev.window.data1, ev.window.data2);
            }
            if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                if (keymapper.GamingMode()) {
                    keymapper.SetGamingMode(false);
                }
            }
            if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                if (ev.key.keysym.scancode == SDL_SCANCODE_F1) {
                    cfg.keymap_edit = !cfg.keymap_edit;
                    if (cfg.keymap_edit && keymapper.GamingMode()) {
                        keymapper.SetGamingMode(false);
                    }
                    continue;
                }
                if (ev.key.keysym.scancode == SDL_SCANCODE_TAB) {
                    if (!cfg.keymap_edit) {
                        keymapper.SetGamingMode(!keymapper.GamingMode());
                    }
                    continue;
                }
                if (ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE && keymapper.GamingMode()) {
                    keymapper.SetGamingMode(false);
                    continue;
                }
                if (ev.key.keysym.scancode == SDL_SCANCODE_F2) {
                    transport.SendControl({.cmd = dexlink::protocol::ControlCmd::NavAction, .arg_u8 = 1});
                    continue;
                }
                if (ev.key.keysym.scancode == SDL_SCANCODE_F11) {
                    cfg.fullscreen = !cfg.fullscreen;
                    SDL_SetWindowFullscreen(window, cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    continue;
                }
            }

            if (cfg.keymap_edit && ui.HandleKeymapEditorEvent(ev, video_rect)) {
                continue;
            }

            const ImGuiIO& io = ImGui::GetIO();
            const bool ui_mouse = io.WantCaptureMouse && !keymapper.GamingMode();
            const bool ui_kbd = io.WantCaptureKeyboard && !keymapper.GamingMode();
            if (!cfg.keymap_edit) {
                keymapper.HandleSdlEvent(ev, video_rect, ui_mouse, ui_kbd);
            }
        }

        keymapper.Tick(SDL_GetTicks());

        // 1-slot Drop-Oldest Frame Pacer: consume only the single latest frame
        if (renderer.UpdateVideoTextureFromSlot(frame_slot)) {
            render_frames++;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - fps_window).count();
        if (elapsed_ms >= 500) {
            float rfps = render_frames * 1000.0f / static_cast<float>(elapsed_ms);
            telemetry.Update([rfps](dexlink::TelemetrySnapshot& s) { s.render_fps = rfps; });
            render_frames = 0;
            fps_window = now;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        bool quit_btn = false;
        ui.Render(window, renderer, video_rect, quit_btn);
        if (quit_btn) running = false;

        ImGui::Render();
        renderer.BeginFrame();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        renderer.EndFrame(cfg.vsync);
    }

    keymapper.ReleaseAll();
    adb.Stop();
    transport.Stop();
    audio_player.Stop();
    decoder.Stop();
    frame_slot.Clear();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    renderer.Shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
