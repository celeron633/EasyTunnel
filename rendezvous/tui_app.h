#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "config.h"
#include "server.h"

class RendezvousTuiApp {
public:
    explicit RendezvousTuiApp(std::string configPath);
    ~RendezvousTuiApp();

    bool Init(std::string* error);
    int Run();

private:
    ftxui::Component BuildDashboardTab();
    ftxui::Component BuildConfigTab();
    ftxui::Component BuildLogTab();
    void StartServer();
    void StopServer();
    void RestartServer();
    bool ReadConfigEditor(RendezvousConfig* config, std::string* error) const;
    void SyncConfigEditor();
    void SaveConfig(bool restart);
    void RefreshSnapshot();
    void OnLog(LogLevel level, const std::string& message);
    void StopTicker();

    ftxui::ScreenInteractive screen_;
    std::string configPath_;
    RendezvousConfig config_;
    RendezvousConfig serverConfig_;
    std::string portText_;
    std::string timeoutText_;
    std::string capacityText_;
    std::vector<std::string> logLevels_{"Debug", "Info", "Warn", "Error"};
    int logLevelIndex_ = 1;
    std::string configMessage_;
    bool configMessageOk_ = true;

    std::vector<std::string> tabs_{"Dashboard", "Config", "Logs"};
    int selectedTab_ = 0;
    int dashboardScroll_ = 0;
    RendezvousServerSnapshot snapshot_;
    std::chrono::steady_clock::time_point serverStarted_{};

    std::atomic<bool> serverRunning_{false};
    std::atomic<int> serverExitCode_{0};
    std::unique_ptr<RendezvousServer> server_;
    std::thread serverThread_;

    std::mutex logMutex_;
    std::vector<std::pair<LogLevel, std::string>> logLines_;
    bool showLogHistory_ = false;

    std::atomic<bool> tickerRunning_{false};
    std::thread tickerThread_;
};
