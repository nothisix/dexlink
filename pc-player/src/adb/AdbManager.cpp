#include "adb/AdbManager.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace dexlink {

namespace {

std::string QuoteArg(const std::string& arg) {
    if (arg.find_first_of(" \t\"") == std::string::npos) {
        return arg;
    }
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
}

std::string BuildCmdLine(const std::string& exe, const std::vector<std::string>& args) {
    std::string cmd = QuoteArg(exe);
    for (const auto& a : args) {
        cmd += ' ';
        cmd += QuoteArg(a);
    }
    return cmd;
}

std::vector<AdbDevice> ParseTrackDevicesPayload(const std::string& payload) {
    std::vector<AdbDevice> list;
    std::istringstream iss(payload);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        AdbDevice d;
        d.serial = line.substr(0, tab);
        d.state = line.substr(tab + 1);
        if (!d.serial.empty()) {
            list.push_back(std::move(d));
        }
    }
    return list;
}

}  // namespace

AdbManager::AdbManager(SharedTelemetry& telemetry) : telemetry_(telemetry) {}

AdbManager::~AdbManager() {
    Stop();
}

std::string AdbManager::FindAdbExecutable() const {
    if (const char* sdk = std::getenv("ANDROID_HOME")) {
        fs::path p = fs::path(sdk) / "platform-tools" / "adb.exe";
        if (fs::exists(p)) return p.string();
    }
    if (const char* local = std::getenv("LOCALAPPDATA")) {
        fs::path p = fs::path(local) / "Android" / "Sdk" / "platform-tools" / "adb.exe";
        if (fs::exists(p)) return p.string();
    }
    return "adb.exe";
}

std::string AdbManager::ResolveServerJarPath() const {
    char exe_buf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe_buf, MAX_PATH);
    fs::path exe_dir = fs::path(exe_buf).parent_path();

    const fs::path candidates[] = {
        exe_dir / "dexlink-server.jar",
        exe_dir / ".." / "android-server" / "build" / "dexlink-server.jar",
        exe_dir / ".." / ".." / "android-server" / "build" / "dexlink-server.jar",
        fs::current_path() / "dexlink-server.jar",
        fs::current_path() / "android-server" / "build" / "dexlink-server.jar",
        fs::current_path() / ".." / "android-server" / "build" / "dexlink-server.jar",
    };

    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) {
            return fs::weakly_canonical(c, ec).string();
        }
    }
    return "";
}

void AdbManager::Start(const AppConfig& initial_cfg) {
    if (running_.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cfg_ = initial_cfg;
        adb_exe_ = (!initial_cfg.adb_path.empty() && initial_cfg.adb_path != "adb")
                       ? initial_cfg.adb_path
                       : FindAdbExecutable();
    }
    track_thread_ = std::thread(&AdbManager::TrackLoop, this);
}

void AdbManager::Stop() {
    if (!running_.exchange(false)) return;
    StopServerProcess();
    std::string serial;
    {
        std::lock_guard<std::mutex> lock(mu_);
        serial = active_serial_;
        active_serial_.clear();
    }
    if (!serial.empty()) {
        CleanupTunnels(serial);
    }
    if (track_thread_.joinable()) {
        track_thread_.join();
    }
    if (server_wait_thread_.joinable()) {
        server_wait_thread_.join();
    }
}

void AdbManager::UpdateConfig(const AppConfig& cfg) {
    std::lock_guard<std::mutex> lock(mu_);
    cfg_ = cfg;
}

void AdbManager::RestartBridge() {
    std::string serial;
    AppConfig cfg;
    {
        std::lock_guard<std::mutex> lock(mu_);
        serial = active_serial_;
        cfg = cfg_;
    }
    if (serial.empty()) return;
    StopServerProcess();
    StartServerForDevice(serial, cfg);
}

void AdbManager::LaunchCompanionApp() {
    std::string serial;
    {
        std::lock_guard<std::mutex> lock(mu_);
        serial = active_serial_;
    }
    if (serial.empty()) return;
    RunAdbSync({"-s", serial, "shell", "am", "start", "-n", "com.dexlink.app/.MainActivity"}, nullptr, 4000);
}

void AdbManager::LaunchPackage(const std::string& package_name) {
    if (package_name.empty()) return;
    std::string serial;
    {
        std::lock_guard<std::mutex> lock(mu_);
        serial = active_serial_;
    }
    if (serial.empty()) return;
    RunAdbSync({"-s", serial, "shell", "monkey", "-p", package_name, "-c", "android.intent.category.LAUNCHER", "1"}, nullptr, 4000);
}

void AdbManager::TrackLoop() {
    while (running_.load()) {
        std::string adb;
        {
            std::lock_guard<std::mutex> lock(mu_);
            adb = adb_exe_;
        }

        // Ensure ADB server daemon is awake before tracking
        RunAdbSync({"start-server"}, nullptr, 5000);

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE read_pipe = nullptr;
        HANDLE write_pipe = nullptr;
        if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }
        SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = write_pipe;
        si.hStdError = write_pipe;

        PROCESS_INFORMATION pi{};
        std::string cmd = BuildCmdLine(adb, {"track-devices"});
        std::vector<char> cmd_buf(cmd.begin(), cmd.end());
        cmd_buf.push_back('\0');

        BOOL ok = CreateProcessA(
            nullptr,
            cmd_buf.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi
        );
        CloseHandle(write_pipe);

        if (!ok) {
            CloseHandle(read_pipe);
            telemetry_.Update([](TelemetrySnapshot& s) {
                s.usb_attached = false;
                s.device_state = "NO ADB";
                s.last_error = "adb.exe launch failed";
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            continue;
        }

        auto read_exact = [&](char* dst, DWORD len) -> bool {
            DWORD got = 0;
            while (got < len && running_.load()) {
                DWORD chunk = 0;
                if (!ReadFile(read_pipe, dst + got, len - got, &chunk, nullptr) || chunk == 0) {
                    return false;
                }
                got += chunk;
            }
            return got == len;
        };

        char len_hex[5]{};
        while (running_.load() && read_exact(len_hex, 4)) {
            len_hex[4] = '\0';
            unsigned long body_len = std::strtoul(len_hex, nullptr, 16);
            if (body_len > 65536) break;
            std::string body(body_len, '\0');
            if (body_len > 0 && !read_exact(body.data(), static_cast<DWORD>(body_len))) {
                break;
            }
            auto devices = ParseTrackDevicesPayload(body);
            OnDeviceListChanged(devices);
        }

        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(read_pipe);

        if (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(750));
        }
    }
}

void AdbManager::OnDeviceListChanged(const std::vector<AdbDevice>& devices) {
    const AdbDevice* chosen = nullptr;
    for (const auto& d : devices) {
        if (d.state == "device") {
            chosen = &d;
            break;
        }
    }

    if (!chosen) {
        std::string prev_serial;
        {
            std::lock_guard<std::mutex> lock(mu_);
            prev_serial = active_serial_;
            active_serial_.clear();
        }
        if (!prev_serial.empty()) {
            StopServerProcess();
        }
        const std::string fallback_state = devices.empty() ? "WAIT" : devices.front().state;
        const std::string fallback_serial = devices.empty() ? "NONE" : devices.front().serial;
        telemetry_.Update([&](TelemetrySnapshot& s) {
            s.usb_attached = !devices.empty();
            s.video_connected = false;
            s.device_serial = fallback_serial;
            s.device_state = fallback_state;
        });
        return;
    }

    std::string serial = chosen->serial;
    bool need_start = false;
    AppConfig cfg;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (active_serial_ != serial || !server_running_.load()) {
            active_serial_ = serial;
            cfg = cfg_;
            need_start = true;
        }
    }

    telemetry_.Update([&](TelemetrySnapshot& s) {
        s.usb_attached = true;
        s.device_serial = serial;
        s.device_state = "BRIDGE";
        s.last_error.clear();
    });

    if (need_start) {
        StartServerForDevice(serial, cfg);
    }
}

void AdbManager::StartServerForDevice(const std::string& serial, const AppConfig& cfg) {
    StopServerProcess();

    const std::string jar_path = ResolveServerJarPath();
    if (jar_path.empty()) {
        telemetry_.Update([](TelemetrySnapshot& s) {
            s.device_state = "ERR";
            s.last_error = "Missing dexlink-server.jar";
        });
        return;
    }

    const std::string p_video = "tcp:" + std::to_string(protocol::kVideoControlPort);
    const std::string p_audio = "tcp:" + std::to_string(protocol::kAudioTelemetryPort);

    if (!RunAdbSync({"-s", serial, "reverse", p_video, p_video}) ||
        !RunAdbSync({"-s", serial, "reverse", p_audio, p_audio})) {
        telemetry_.Update([](TelemetrySnapshot& s) {
            s.device_state = "ERR";
            s.last_error = "adb reverse failed";
        });
        return;
    }

    if (!RunAdbSync({"-s", serial, "push", jar_path, "/data/local/tmp/dexlink-server.jar"}, nullptr, 15000)) {
        telemetry_.Update([](TelemetrySnapshot& s) {
            s.device_state = "ERR";
            s.last_error = "adb push server jar failed";
        });
        return;
    }

    std::string adb;
    {
        std::lock_guard<std::mutex> lock(mu_);
        adb = adb_exe_;
    }

    std::ostringstream remote_cmd;
    remote_cmd << "CLASSPATH=/data/local/tmp/dexlink-server.jar app_process / com.dexlink.server.Main"
               << " --host 127.0.0.1"
               << " --port " << protocol::kVideoControlPort
               << " --width " << cfg.width
               << " --height " << cfg.height
               << " --fps " << cfg.fps
               << " --bitrate " << (cfg.bitrate_mbps * 1000000)
               << " --dpi " << cfg.dpi
               << " --mode " << (cfg.mode == protocol::DisplayMode::Dex ? "dex" : "mirror")
               << " --codec " << (cfg.codec == protocol::VideoCodec::Hevc ? "hevc" : "h264");
    if (!cfg.launch_package.empty()) {
        remote_cmd << " --pkg " << cfg.launch_package;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    std::string full_cmd = BuildCmdLine(adb, {"-s", serial, "shell", remote_cmd.str()});
    std::vector<char> cmd_buf(full_cmd.begin(), full_cmd.end());
    cmd_buf.push_back('\0');

    if (!CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        telemetry_.Update([](TelemetrySnapshot& s) {
            s.device_state = "ERR";
            s.last_error = "Failed launching app_process";
        });
        return;
    }

    CloseHandle(pi.hThread);
    {
        std::lock_guard<std::mutex> lock(mu_);
        server_process_ = pi.hProcess;
        server_running_.store(true);
    }

    if (server_wait_thread_.joinable()) {
        server_wait_thread_.join();
    }
    HANDLE h = pi.hProcess;
    server_wait_thread_ = std::thread([this, h]() {
        WaitForSingleObject(h, INFINITE);
        server_running_.store(false);
    });
}

void AdbManager::StopServerProcess() {
    HANDLE proc = nullptr;
    std::string serial;
    {
        std::lock_guard<std::mutex> lock(mu_);
        proc = server_process_;
        server_process_ = nullptr;
        serial = active_serial_;
    }
    server_running_.store(false);
    if (proc) {
        TerminateProcess(proc, 0);
        WaitForSingleObject(proc, 1000);
        CloseHandle(proc);
    }
    if (!serial.empty()) {
        RunAdbSync({"-s", serial, "shell", "pkill -f com.dexlink.server.Main"}, nullptr, 2000);
    }
}

void AdbManager::CleanupTunnels(const std::string& serial) {
    RunAdbSync({"-s", serial, "reverse", "--remove", "tcp:" + std::to_string(protocol::kVideoControlPort)}, nullptr, 2000);
    RunAdbSync({"-s", serial, "reverse", "--remove", "tcp:" + std::to_string(protocol::kAudioTelemetryPort)}, nullptr, 2000);
}

bool AdbManager::RunAdbSync(const std::vector<std::string>& args, std::string* stdout_out, int timeout_ms) {
    std::string adb;
    {
        std::lock_guard<std::mutex> lock(mu_);
        adb = adb_exe_;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (stdout_out) {
        CreatePipe(&read_pipe, &write_pipe, &sa, 0);
        SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | (stdout_out ? STARTF_USESTDHANDLES : 0);
    si.wShowWindow = SW_HIDE;
    if (stdout_out) {
        si.hStdOutput = write_pipe;
        si.hStdError = write_pipe;
    }

    PROCESS_INFORMATION pi{};
    std::string cmd = BuildCmdLine(adb, args);
    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');

    BOOL ok = CreateProcessA(
        nullptr,
        cmd_buf.data(),
        nullptr,
        nullptr,
        stdout_out ? TRUE : FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );
    if (write_pipe) CloseHandle(write_pipe);

    if (!ok) {
        if (read_pipe) CloseHandle(read_pipe);
        return false;
    }

    if (stdout_out && read_pipe) {
        char buf[1024];
        DWORD n = 0;
        while (ReadFile(read_pipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
            stdout_out->append(buf, n);
        }
        CloseHandle(read_pipe);
    }

    DWORD wait_res = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout_ms));
    DWORD exit_code = 1;
    if (wait_res == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
    } else {
        GetExitCodeProcess(pi.hProcess, &exit_code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return wait_res == WAIT_OBJECT_0 && exit_code == 0;
}

}  // namespace dexlink
