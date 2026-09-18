#include "main_window.h"

#include <filesystem>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QWindowStateChangeEvent>

#include "../log.h"
#include "gui_theme.h"
namespace {
constexpr int kTickIntervalMs = 100;

const char* const kTrayIconPaths[] = {
    ":/icons/EasyTunnel.ico",
    ":/icons/IconSets/EasyTunnelDisconnected.ico",
    ":/icons/IconSets/EasyTunnelRx.ico",
    ":/icons/IconSets/EasyTunnelTx.ico",
    ":/icons/IconSets/EasyTunnelRxTx.ico",
};

QString BadgeStyle(const QColor& color) {
    QColor background = color;
    background.setAlphaF(0.12f);
    return QStringLiteral(
               "QLabel { color: %1; background-color: rgba(%2, %3, %4, %5);"
               " border-radius: 4px; padding: 3px 8px; font-weight: 600; }")
        .arg(color.name())
        .arg(background.red())
        .arg(background.green())
        .arg(background.blue())
        .arg(background.alphaF());
}
}  // namespace

MainWindow::MainWindow() {
    SetLogCallback([this](LogLevel level, const std::string& message) {
        OnLog(level, message);
    });
    try {
        configFilePath_ = std::filesystem::absolute(kClientConfigFileName).string();
    } catch (...) {
        configFilePath_ = kClientConfigFileName;
    }
    bool configLoaded = false;
    const std::string loadMessage = LoadGuiConfig(&configLoaded);

    setWindowTitle(QStringLiteral("EasyTunnel"));
    // Wide enough for the two-column settings page without horizontal scrolling.
    resize(1040, 680);

    // Created first: building the tabs already refreshes the state widgets.
    statusLabel_ = new QLabel(this);
    statusBar()->addWidget(statusLabel_, 1);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(16, 12, 16, 8);
    layout->addWidget(BuildHeader());
    auto* tabs = new QTabWidget(central);
    tabs->addTab(BuildConnectionTab(), QStringLiteral("Connection"));
    tabs->addTab(BuildSettingsTab(), QStringLiteral("Settings"));
    tabs->addTab(BuildLogTab(), QStringLiteral("Log"));
    layout->addWidget(tabs, 1);
    setCentralWidget(central);

    BuildTray();
    ShowConfigSaveMessage(loadMessage, configLoaded);

    engine_.SetStateCallback([this](TunnelState state, const std::string& message) {
        OnStateChanged(state, message);
    });
    autoWaitEnabledRuntime_.store(config_.autoWaitForPeer);
    autoWaitRetryDelaySecondsRuntime_.store(config_.rendezvousRetryDelaySeconds);
    autoWaitPending_.store(config_.autoWaitForPeer);

    tickTimer_ = new QTimer(this);
    connect(tickTimer_, &QTimer::timeout, this, [this] { OnTick(); });
    tickTimer_->start(kTickIntervalMs);
    OnTick();
}

MainWindow::~MainWindow() {
    shuttingDown_.store(true);
    autoWaitPending_.store(false);
    autoWaitEnabledRuntime_.store(false);
    if (tickTimer_) tickTimer_->stop();
    JoinStunDiagnostic();
    engine_.Stop();
    SetLogCallback({});
    if (tray_) tray_->hide();
}

QWidget* MainWindow::BuildHeader() {
    auto* header = new QWidget(this);
    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(4, 2, 4, 6);
    auto* title = new QLabel(QStringLiteral("EasyTunnel"), header);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.35);
    title->setFont(titleFont);
    layout->addWidget(title);
    layout->addSpacing(8);
    stateBadge_ = new QLabel(header);
    layout->addWidget(stateBadge_);
    layout->addStretch(1);
    identityLabel_ = new QLabel(header);
    identityLabel_->setStyleSheet(gui_theme::TextColorStyle(gui_theme::kMuted));
    layout->addWidget(identityLabel_);
    UpdateIdentityLabel();
    return header;
}

void MainWindow::UpdateIdentityLabel() {
    identityLabel_->setText(QString::fromStdString(config_.roomId + " / " + config_.peerId));
}

void MainWindow::BuildTray() {
    for (std::size_t index = 0; index < trayIcons_.size(); ++index) {
        trayIcons_[index] = QIcon(QString::fromLatin1(kTrayIconPaths[index]));
    }
    const QIcon& appIcon = trayIcons_[static_cast<std::size_t>(TrayMode::Idle)];
    if (!appIcon.isNull()) {
        QApplication::setWindowIcon(appIcon);
        setWindowIcon(appIcon);
    }
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;

    tray_ = new QSystemTrayIcon(this);
    auto* menu = new QMenu(this);
    gui_theme::PrepareMenu(menu);
    // A read-only status line, so the state is visible without opening the window.
    trayStatusLabel_ = new QLabel(menu);
    trayStatusLabel_->setContentsMargins(16, 6, 16, 6);
    auto* statusAction = new QWidgetAction(menu);
    statusAction->setDefaultWidget(trayStatusLabel_);
    menu->addAction(statusAction);
    menu->addSeparator();
    menu->addAction(QStringLiteral("Show window"), this, [this] { ShowFromTray(); });
    trayDisconnectAction_ = menu->addAction(QStringLiteral("Disconnect"), this, [this] {
        Disconnect();
        stateDirty_.store(true);
    });
    menu->addSeparator();

    // Mirrors of the Settings switches. Toggling one goes through the switch,
    // so saving, the runtime auto-wait state and the scheduled task behave
    // exactly as in Settings; the item then re-reads the switch, which the
    // startup handler may have reverted.
    auto addToggle = [this, menu](const QString& text, QCheckBox* source) {
        QAction* action = menu->addAction(text);
        action->setCheckable(true);
        connect(action, &QAction::triggered, this, [action, source](bool checked) {
            source->setChecked(checked);
            action->setChecked(source->isChecked());
        });
        return action;
    };
    trayAutoWaitAction_ = addToggle(QStringLiteral("Auto wait for peer"), autoWaitCheck_);
#ifdef _WIN32
    trayStartWithWindowsAction_ =
        addToggle(QStringLiteral("Start with Windows"), startWithWindowsCheck_);
#endif
    connect(menu, &QMenu::aboutToShow, this, [this] {
        trayAutoWaitAction_->setChecked(autoWaitCheck_->isChecked());
#ifdef _WIN32
        trayStartWithWindowsAction_->setChecked(startWithWindowsCheck_->isChecked());
#endif
    });
    menu->addSeparator();
    menu->addAction(QStringLiteral("Exit"), this, [this] { RequestExit(); });
    tray_->setContextMenu(menu);
    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger
                    || reason == QSystemTrayIcon::DoubleClick) {
                    ShowFromTray();
                }
            });
    UpdateTray(false, false);
    tray_->show();
}

// Changes both the notification-area icon and the window/taskbar icon.
// Disconnected, Waiting and Error all use the disconnected artwork.
void MainWindow::UpdateTray(bool rxActive, bool txActive) {
    const TunnelState state = currentState_.load();
    const bool unavailable = state == TunnelState::Disconnected
        || state == TunnelState::Waiting || state == TunnelState::Error;
    TrayMode mode = TrayMode::Idle;
    if (unavailable) mode = TrayMode::Disconnected;
    else if (rxActive && txActive) mode = TrayMode::RxTx;
    else if (rxActive) mode = TrayMode::Rx;
    else if (txActive) mode = TrayMode::Tx;
    if (mode == trayMode_) return;
    trayMode_ = mode;

    QIcon icon = trayIcons_[static_cast<std::size_t>(mode)];
    if (icon.isNull()) icon = trayIcons_[static_cast<std::size_t>(TrayMode::Idle)];
    if (icon.isNull()) return;
    setWindowIcon(icon);
    if (tray_) {
        tray_->setIcon(icon);
        tray_->setToolTip(unavailable ? QStringLiteral("EasyTunnel - disconnected / waiting")
                                      : QStringLiteral("EasyTunnel - connected"));
    }
}

void MainWindow::OnTick() {
    ProcessAutoWait();
    if (stateDirty_.exchange(false)
        || renderedState_ != currentState_.load()
        || renderedWaiting_ != waitingForPeer_.load()) {
        RefreshStateUi();
    }
    UpdateLiveStats();
    UpdateTrafficUi();
    statisticsHistory_.Update(engine_.GetStats().txBytes.load(),
                              engine_.GetStats().rxBytes.load(),
                              engine_.GetStats().rttMilliseconds.load());
    UpdateCharts();
    DrainLog();
    if (stunDiagnosticDirty_.exchange(false)) UpdateStunDiagnosticUi();
}

void MainWindow::RefreshStateUi() {
    const TunnelState state = currentState_.load();
    const bool waiting = IsTunnelActive(state) && waitingForPeer_.load();
    renderedState_ = state;
    renderedWaiting_ = waitingForPeer_.load();

    const gui_theme::StateStyle style = gui_theme::StyleFor(state);
    stateBadge_->setText(QString::fromLatin1(style.label));
    stateBadge_->setStyleSheet(BadgeStyle(style.color));
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        statusLabel_->setText(QString::fromStdString(statusMessage_));
    }
    statusLabel_->setStyleSheet(gui_theme::TextColorStyle(style.color));

    const bool active = IsTunnelActive(state);
    const bool canBrowseClients = !active || waiting;
    refreshButton_->setEnabled(canBrowseClients);
    waitButton_->setText(active ? QStringLiteral("Disconnect")
                                : QStringLiteral("Wait for peer"));
    gui_theme::SetButtonVariant(waitButton_, active ? "danger" : "primary");
    peerTable_->setSelectionMode(canBrowseClients ? QAbstractItemView::SingleSelection
                                                  : QAbstractItemView::NoSelection);
    const int row = peerTable_->currentRow();
    const bool hasSelection = row >= 0 && row < static_cast<int>(clients_.size())
        && !peerTable_->selectedItems().isEmpty();
    connectButton_->setEnabled(hasSelection && (!active || waiting));
    for (QWidget* field : rendezvousIdentityFields_) field->setEnabled(!active);

    // The tray is built after the tabs, which already refresh this once.
    if (trayStatusLabel_) {
        trayStatusLabel_->setText(QString::fromLatin1(style.label));
        trayStatusLabel_->setStyleSheet(gui_theme::TextColorStyle(style.color)
            + QStringLiteral(" font-weight: 600;"));
        trayDisconnectAction_->setEnabled(active);
    }
}

void MainWindow::ShowFromTray() {
    showNormal();
    raise();
    activateWindow();
}

void MainWindow::RequestExit() {
    ShowFromTray();
    if (!ConfirmExit()) return;
    exitConfirmed_ = true;
    close();
}

bool MainWindow::ConfirmExit() {
    QMessageBox box(QMessageBox::Question, QStringLiteral("Exit EasyTunnel?"),
                    QStringLiteral("Are you sure you want to exit?"),
                    QMessageBox::Yes | QMessageBox::No, this);
    box.setDefaultButton(QMessageBox::No);
    return box.exec() == QMessageBox::Yes;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!exitConfirmed_) {
        if (tray_ && config_.closeToMinimize) {
            hide();
            event->ignore();
            return;
        }
        if (!ConfirmExit()) {
            event->ignore();
            return;
        }
        exitConfirmed_ = true;
    }
    shuttingDown_.store(true);
    autoWaitPending_.store(false);
    if (saveTimer_ && saveTimer_->isActive()) {
        saveTimer_->stop();
        SaveGuiConfig();
    }
    Disconnect();
    if (tray_) tray_->hide();
    event->accept();
    QApplication::quit();
}

// Minimizing sends the window to the notification area when there is one.
void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() != QEvent::WindowStateChange || !tray_) return;
    if (isMinimized()) QTimer::singleShot(0, this, [this] { hide(); });
}
