#pragma once

#include "core/Config.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace dexlink {

struct AdbDevice {
    std::string serial;
    std::string state;
};

struct AndroidAppInfo {
    std::string package_name;
    std::string short_name;
    std::string monogram;
    bool is_running = false;
};

class AdbManager {
public:
    explicit AdbManager(SharedTelemetry& telemetry);
    ~AdbManager();

    AdbManager(const AdbManager&) = delete;
    AdbManager& operator=(const AdbManager&) = delete;

    void Start(const AppConfig& initial_cfg);
    void Stop();

    void UpdateConfig(const AppConfig& cfg);
    void LaunchCompanionApp();
    void LaunchPackage(const std::string& package_name);
    void RestartBridge();

    void RefreshAppsAsync();
    std::vector<AndroidAppInfo> GetInstalledApps() const;
    void MarkAppRunning(const std::string& package_name);
    std::string ActivePackage() const;

    std::string ResolveServerJarPath() const;

private:
    void TrackLoop();
    void OnDeviceListChanged(const std::vector<AdbDevice>& devices);
    void StartServerForDevice(const std::string& serial, const AppConfig& cfg);
    void StopServerProcess();
    void CleanupTunnels(const std::string& serial);

    bool RunAdbSync(const std::vector<std::string>& args, std::string* stdout_out = nullptr, int timeout_ms = 8000);
    std::string FindAdbExecutable() const;

    SharedTelemetry& telemetry_;
    std::atomic<bool> running_{false};
    std::thread track_thread_;
    std::thread server_wait_thread_;
    std::thread apps_thread_;

    mutable std::mutex mu_;
    AppConfig cfg_{};
    std::string active_serial_;
    HANDLE server_process_ = nullptr;
    std::atomic<bool> server_running_{false};
    std::string adb_exe_;

    mutable std::mutex apps_mu_;
    std::vector<AndroidAppInfo> installed_apps_;
    std::vector<std::string> running_packages_;
    std::string active_package_;
    std::atomic<bool> apps_fetching_{false};
};

}  // namespace dexlink
