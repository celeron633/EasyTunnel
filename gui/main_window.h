#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <QIcon>
#include <QMainWindow>

#include "../client_config.h"
#include "../rendezvous_client.h"
#include "../statistics_history.h"
#include "../tunnel_engine.h"

class HistoryChart;
class QCheckBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QSystemTrayIcon;
class QTableWidget;
class QTimer;

class MainWindow : public QMainWindow {
public:
    MainWindow();
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    enum class TrayMode : std::size_t { Idle, Disconnected, Rx, Tx, RxTx, Count };

    // Construction (main_window.cpp and the per-tab files).
    QWidget* BuildHeader();
    QWidget* BuildConnectionTab();
    QWidget* BuildSettingsTab();
    QWidget* BuildLogTab();
    void BuildTray();

    // Periodic UI refresh. Engine and log callbacks arrive on worker threads;
    // they only touch atomics and mutex-guarded buffers, and this 100 ms tick
    // moves the results into the widgets on the UI thread.
    void OnTick();
    void RefreshStateUi();
    void UpdateLiveStats();
    void UpdateTrafficUi();
    void UpdateCharts();
    void UpdateTray(bool rxActive, bool txActive);
    void DrainLog();
    void UpdateLogLineCount();

    // Connection handling.
    bool StartConnection(const std::string& targetPeerId);
    void ConnectSelectedClient();
    void Disconnect();
    void RefreshClients();
    void RebuildPeerTable();
    void ProcessAutoWait();
    void OnStateChanged(TunnelState state, const std::string& message);
    void SetStatusMessage(const std::string& message);
    void OnLog(LogLevel level, const std::string& message);

    // Settings.
    QLineEdit* AddTextField(QFormLayout* form, const QString& label,
                            std::string* target, bool password = false);
    QSpinBox* AddIntField(QFormLayout* form, const QString& label, int* target,
                          int minimum, int maximum,
                          std::function<void()> onChanged = {});
    QCheckBox* AddCheckField(QFormLayout* form, const QString& label, bool* target,
                             std::function<void()> onChanged = {});
    void RebuildTraversalTable();
    void OnAutoWaitChanged();
    void UpdateIdentityLabel();
    std::string LoadGuiConfig(bool* succeeded);
    bool SaveGuiConfig();
    void ScheduleConfigSave();
    void ShowConfigSaveMessage(const std::string& message, bool succeeded);
    void StartStunDiagnostic();
    void UpdateStunDiagnosticUi();
    void JoinStunDiagnostic();

    // Window and tray behaviour.
    void ShowFromTray();
    void RequestExit();
    bool ConfirmExit();

    TunnelEngine engine_;
    ClientConfig config_;
    std::string configFilePath_;
    std::vector<RendezvousPeerInfo> clients_;

    QTimer* tickTimer_ = nullptr;
    QTimer* saveTimer_ = nullptr;
    QTimer* saveMessageTimer_ = nullptr;

    // Header and status bar.
    QLabel* stateBadge_ = nullptr;
    QLabel* identityLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;

    // Connection tab.
    QPushButton* refreshButton_ = nullptr;
    QPushButton* waitButton_ = nullptr;
    QPushButton* connectButton_ = nullptr;
    QLabel* onlineLabel_ = nullptr;
    QTableWidget* peerTable_ = nullptr;
    QTableWidget* trafficTable_ = nullptr;
    // Activity shown by each row's dot icon: -1 unknown, 0 idle, 1 active.
    std::array<int, 2> trafficRowActive_{-1, -1};
    QLabel* latencyLabel_ = nullptr;
    std::array<HistoryChart*, 3> charts_{};

    // Settings tab.
    std::vector<QWidget*> rendezvousIdentityFields_;
    QLabel* mtuWarning_ = nullptr;
    QTableWidget* traversalTable_ = nullptr;
    QPushButton* stunTestButton_ = nullptr;
    QLabel* stunResultLabel_ = nullptr;
    QLabel* configSaveLabel_ = nullptr;
#ifdef _WIN32
    QCheckBox* startWithWindowsCheck_ = nullptr;
#endif

    // Log tab.
    QPlainTextEdit* logView_ = nullptr;
    QCheckBox* logAutoScroll_ = nullptr;
    QLabel* logLineCount_ = nullptr;

    // Tray.
    QSystemTrayIcon* tray_ = nullptr;
    std::array<QIcon, static_cast<std::size_t>(TrayMode::Count)> trayIcons_{};
    TrayMode trayMode_ = TrayMode::Count;
    bool exitConfirmed_ = false;

    std::mutex statusMutex_;
    std::string statusMessage_ = "Disconnected";
    std::atomic<TunnelState> currentState_{TunnelState::Disconnected};
    std::atomic<bool> stateDirty_{true};
    TunnelState renderedState_ = TunnelState::Disconnected;
    bool renderedWaiting_ = false;

    std::mutex logMutex_;
    std::vector<std::string> pendingLogLines_;

    std::thread stunDiagnosticThread_;
    std::atomic<bool> stunDiagnosticRunning_{false};
    std::atomic<bool> stunDiagnosticDirty_{false};
    std::mutex stunDiagnosticMutex_;
    std::string stunDiagnosticMessage_ = "Not tested";
    bool stunDiagnosticCompleted_ = false;
    bool stunDiagnosticSucceeded_ = false;

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
    std::size_t renderedSampleCount_ = 0;
    std::chrono::system_clock::time_point renderedSampleTime_{};

    std::atomic<bool> autoWaitEnabledRuntime_{false};
    std::atomic<bool> autoWaitPending_{false};
    std::atomic<bool> suppressAutoWait_{false};
    std::atomic<bool> shuttingDown_{false};
    std::atomic<bool> waitingForPeer_{false};
    std::atomic<int> autoWaitRetryDelaySecondsRuntime_{5};
    std::atomic<int64_t> autoWaitRetryAfterMs_{0};
};
