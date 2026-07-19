#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

#include "../tunnel_engine.h"

struct GLFWwindow;
#ifdef _WIN32
class DisconnectConfirmationDialog;
class ExitConfirmationDialog;
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
    bool ValidateCommonFields(std::string* error) const;
    bool LoadGuiConfig();
    bool SaveGuiConfig();
    void ShowConfigSaveMessage(std::string message, bool succeeded);
    void RenderConfigSaveStatus();
    void UpdateLiveStats();
    void ProcessAutoWait();

    GLFWwindow* window_ = nullptr;
    std::unique_ptr<UiHeartbeat> uiHeartbeat_;
#ifdef _WIN32
    std::unique_ptr<WindowsTray> windowsTray_;
    std::unique_ptr<DisconnectConfirmationDialog> disconnectConfirmationDialog_;
    std::unique_ptr<ExitConfirmationDialog> exitConfirmationDialog_;
#endif
    TunnelEngine engine_;

    char serverAddress_[256] = "127.0.0.1";
    int serverPort_ = 3478;
    char roomId_[128] = "default-room";
    char peerId_[128] = "node-a";
    char authToken_[128] = {};
    std::vector<std::string> clients_;
    int selectedClient_ = -1;

    char adapterName_[128] = "EasyTunnel";
    char localTunIpv4_[64] = "10.66.0.1";
    int tunPrefix_ = 24;
    int tunMtu_ = 1452;
    bool autoConfigIpv4_ = true;
    int keepaliveInterval_ = 15;
    int peerTimeout_ = 45;
    bool dummyTrafficEnabled_ = false;
    int punchTimeout_ = 30;
    int nat4SourcePortStart_ = 30000;
    int nat4SourcePortCount_ = 25;
    int nat4PeerPortOffset_ = 20;
    int nat4RoundTimeout_ = 10;
    int logLevelIdx_ = 1;
    bool autoWaitForPeer_ = false;

    std::mutex statusMutex_;
    std::string statusMessage_ = "Disconnected";
    std::atomic<TunnelState> currentState_{TunnelState::Disconnected};
    std::mutex logMutex_;
    std::vector<std::string> logLines_;
    bool logAutoScroll_ = true;
    std::size_t renderedLogLineCount_ = 0;
    std::string configFilePath_;
    std::string configSaveMessage_;
    bool configSaveSucceeded_ = true;
    std::chrono::steady_clock::time_point configSaveMessageExpiresAt_{};

    int txTotalUnit_ = 0;
    int rxTotalUnit_ = 0;
    int txSpeedUnit_ = 0;
    int rxSpeedUnit_ = 0;
    int statusUnit_ = 0;
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
    std::atomic<bool> autoWaitEnabledRuntime_{false};
    std::atomic<bool> autoWaitPending_{false};
    std::atomic<bool> suppressAutoWait_{false};
    std::atomic<bool> shuttingDown_{false};
    std::atomic<bool> waitingForPeer_{false};
    std::atomic<int64_t> autoWaitRetryAfterMs_{0};
};
