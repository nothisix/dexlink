# <img src="brand/dexlink-mark-dark.svg" width="28" height="28" align="absmiddle" alt="DEXLINK" /> DEXLINK

Universal zero-latency USB-C Android-to-PC display, raw PCM audio, and gaming input bridge.

- **Official Download Portal:** https://dexlink.7ust.store
- **Releases & Checksums:** [`releases/`](releases/) (`dexlink-win64-v1.0.0.exe`, `dexlink-android-v1.0.0.apk`, `dexlink-server-v1.0.0.jar`, `dexlink-source-v1.0.0.zip`, `SHA256SUMS.txt`)
- **Brand Assets:** [`brand/`](brand/) (`dexlink-mark-forest.svg`, `dexlink-mark-espresso.svg`, `dexlink-mark-dark.svg`, `dexlink-mark-mono.svg`)

## Repository Layout
- `protocol/` — Little-endian 12-byte header binary protocol (C++20 + Kotlin)
- `android-server/` — ADB `app_process` virtual display, `MediaCodec`, `InputManager`, and screen-off bridge
- `android-app/` — Minimal companion APK for `AudioPlaybackCapture` 48kHz PCM streaming and thermal telemetry
- `pc-player/` — Windows C++20 / SDL2 / D3D11VA / FFmpeg / WASAPI / Dear ImGui client & gaming keymapper
- `profiles/` — Per-package JSON gaming keymaps
- `releases/` — Packaged final binaries, source archive, and `SHA256SUMS.txt`

## Build from Source

### 1. Android Server (`dexlink-server.jar`)
```powershell
cd android-server
.\gradlew.bat test buildServerJar
```

### 2. Android Companion APK (`app-debug.apk`)
```powershell
cd android-app
.\gradlew.bat assembleDebug
```

### 3. Windows PC Player (`dexlink.exe`)
```powershell
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"
cmake -S pc-player -B pc-player/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build pc-player/build -j
```

## Run
1. Connect Android device via USB-C with USB Debugging enabled.
2. Run `dexlink.exe` (automatically runs `adb reverse`, pushes `dexlink-server.jar`, and opens DeX/Mirror display).
3. Install `dexlink-android-v1.0.0.apk` on Android and tap `GRANT AUDIO` for 48kHz raw PCM audio.
4. Hotkeys: `Tab` or `Middle-Click` toggles Gaming Mode cursor lock (`Esc` or `Tab` unlocks cursor); `Right-Click` sends Back; `F2` returns Home; `F1` opens the Keymap Editor; `F11` toggles fullscreen.
