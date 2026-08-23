#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include "../client_config.h"
#include "../rendezvous_client.h"
#include "../tunnel_engine.h"
#include "../statistics_history.h"

#ifdef _WIN32
class TuiWindowsTray;
#endif

class TuiApp {
public:
    TuiApp();
    ~TuiApp();

    bool Init();
    int Run();

private:
    ftxui::Component BuildConnectionTab();
    ftxui::Component BuildSettingsTab();
    ftxui::Component BuildLogTab();
    ftxui::Element RenderStateBadge() const;
    bool StartConnection(const std::string& targetPeerId);
    void ConnectSelectedClient();
    void Disconnect();
    void RefreshClients();
    void UpdateClientLabels();
    void OnStateChanged(TunnelState state, const std::string& message);
    void OnLog(LogLevel level, const std::string& message);
    void SetStatus(const std::string& message);
    void ProcessAutoWait();
    void UpdateStats();
    void UpdateStatisticsHistory();
    ftxui::Element RenderStatisticsCharts() const;
    void UpdateDisplayLabels();
    void SaveIfChanged();
    void CopyAllLogs();
    void CopySelectedText();
    std::string ConfigSignature() const;
    void SyncTextFromConfig();
    void SyncConfigFromText();
    void StopTicker();
    void StartStunDiagnostic();
    void JoinStunDiagnostic();

    ftxui::ScreenInteractive screen_;
#ifdef _WIN32
    std::unique_ptr<TuiWindowsTray> windowsTray_;
#endif
    TunnelEngine engine_;
    ClientConfig config_;
    std::string configPath_;
    std::string savedSignature_;
    std::string configMessage_;
    bool configSaveOk_ = true;

    std::string serverPortText_;
    std::string tunPrefixText_;
    std::string tunMtuText_;
    std::string keepaliveText_;
    std::string peerTimeoutText_;
    std::string punchTimeoutText_;
    std::string natPunchAttemptLimitText_;
    int natPunchProfileIndex_ = 0;
    std::string stunAPortText_;
    std::string stunBPortText_;
    std::string ipv6ListenPortText_;
    std::string ipv6ProbePortText_;
    std::string ipv6FallbackTimeoutText_;
    std::string rendezvousRetryDelayText_;

    std::vector<RendezvousPeerInfo> clientDetails_;
    std::vector<std::string> clients_;
    std::string clientHeader_;
    int selectedClient_ = 0;
    std::vector<std::string> tabs_{"Connection", "Settings", "Log"};
    int selectedTab_ = 0;
    std::vector<std::string> logLevels_{"Debug", "Info", "Warn", "Error"};
    std::vector<std::string> natPunchProfiles_{
        NatPunchProfileDisplayName(NatPunchProfile::Balanced),
        NatPunchProfileDisplayName(NatPunchProfile::Aggressive)};

    std::mutex statusMutex_;
    std::string status_ = "Disconnected";
    std::atomic<TunnelState> state_{TunnelState::Disconnected};
    std::mutex logMutex_;
    std::vector<std::pair<LogLevel, std::string>> logLines_;
    std::string logCopyMessage_;
    bool logCopyOk_ = true;
    std::atomic<int64_t> lastLogRedrawMs_{0};
    std::thread stunDiagnosticThread_;
    std::atomic<bool> stunDiagnosticRunning_{false};
    std::mutex stunDiagnosticMutex_;
    std::string stunDiagnosticMessage_ = "Not tested";
    bool stunDiagnosticCompleted_ = false;
    bool stunDiagnosticSucceeded_ = false;

    int txTotalUnit_ = 0;
    int rxTotalUnit_ = 0;
    int txSpeedUnit_ = 0;
    int rxSpeedUnit_ = 0;
    std::string txTotalLabel_;
    std::string rxTotalLabel_;
    std::string txSpeedLabel_;
    std::string rxSpeedLabel_;
    bool speedInitialized_ = false;
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

    std::atomic<bool> exiting_{false};
    std::atomic<int> retryDelaySeconds_{5};
    std::atomic<int64_t> retryAfterMs_{0};
    std::atomic<bool> tickerRunning_{false};
    std::thread tickerThread_;
};
