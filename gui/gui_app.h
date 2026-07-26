#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

#include "../client_config.h"
#include "../rendezvous_client.h"
#include "../tunnel_engine.h"
#include "../statistics_history.h"

struct GLFWwindow;
#ifdef _WIN32
class WindowsTray;
#endif
class UiHeartbeat;

class GuiApp {
public:
    static constexpr const char* kLogLevels[] = {"Debug", "Info", "Warn", "Error"};
    static constexpr int kLogLevelCount = 4;

    GuiApp();
    ~GuiApp();

    bool Init();
    void Run();
    void Shutdown();

private:
    void RenderFrame();
    void RenderHeader();
    void RenderConnectionTab();
    void RenderSettingsTab();
    void RenderLogTab();
    void RenderStatusBar();
    bool StartConnection(const std::string& targetPeerId);
    void ConnectSelectedClient();
    void Disconnect();
    void RefreshClients();
    void OnStateChanged(TunnelState state, const std::string& message);
    void SetStatusMessage(const std::string& message);
    void OnLog(LogLevel level, const std::string& message);
    bool LoadGuiConfig();
    bool SaveGuiConfig();
    void ShowConfigSaveMessage(std::string message, bool succeeded);
    void RenderConfigSaveStatus();
    void UpdateLiveStats();
    void UpdateStatisticsHistory();
    void RenderStatisticsCharts();
    void ProcessAutoWait();

    GLFWwindow* window_ = nullptr;
    std::unique_ptr<UiHeartbeat> uiHeartbeat_;
#ifdef _WIN32
    std::unique_ptr<WindowsTray> windowsTray_;
#endif
    TunnelEngine engine_;

    // The settings widgets bind straight into this struct; every accepted edit
    // is clamped in RenderSettingsTab and saved through the shared module.
    ClientConfig config_;
    std::vector<RendezvousPeerInfo> clients_;
    int selectedClient_ = -1;

    std::mutex statusMutex_;
    std::string statusMessage_ = "Disconnected";
    std::atomic<TunnelState> currentState_{TunnelState::Disconnected};
    std::mutex logMutex_;
    std::vector<std::string> logLines_;
    bool logAutoScroll_ = true;
    // Rebuilt only when the line count moves, so an idle log tab costs nothing.
    std::string logText_;
    std::size_t renderedLogLineCount_ = 0;
    std::string configFilePath_;
    std::string configSaveMessage_;
    bool configSaveSucceeded_ = true;
    std::chrono::steady_clock::time_point configSaveMessageExpiresAt_{};

    int statisticsTotalUnit_ = 0;
    int statisticsSpeedUnit_ = 0;
    bool speedSampleInitialized_ = false;
    uint64_t previousTxBytes_ = 0;
    uint64_t previousRxBytes_ = 0;
    double txBytesPerSecond_ = 0.0;
    double rxBytesPerSecond_ = 0.0;
    std::chrono::steady_clock::time_point lastSpeedSample_{};
    uint64_t observedTxPackets_ = 0;
    uint64_t observedRxPackets_ = 0;
    std::chrono::steady_clock::time_point lastTxActivity_{};
    std::chrono::steady_clock::time_point lastRxActivity_{};
    StatisticsHistory statisticsHistory_;
    std::atomic<bool> autoWaitEnabledRuntime_{false};
    std::atomic<bool> autoWaitPending_{false};
    std::atomic<bool> suppressAutoWait_{false};
    std::atomic<bool> shuttingDown_{false};
    std::atomic<bool> waitingForPeer_{false};
    std::atomic<int> autoWaitRetryDelaySecondsRuntime_{5};
    std::atomic<int64_t> autoWaitRetryAfterMs_{0};
};
