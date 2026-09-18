#include "main_window.h"

#include <algorithm>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "../log.h"
#include "../stun_client.h"
#include "gui_theme.h"
#ifdef _WIN32
#include "windows_startup.h"
#endif

namespace {
constexpr int kFormLabelWidth = 155;
constexpr int kConfigSaveDelayMs = 400;
constexpr int kConfigSaveMessageMs = 3000;
const char* const kLogLevels[] = {"Debug", "Info", "Warn", "Error"};

QFormLayout* AddSection(QVBoxLayout* column, const QString& title) {
    auto* box = new QGroupBox(title);
    auto* form = new QFormLayout(box);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    column->addWidget(box);
    return form;
}

void AddRow(QFormLayout* form, const QString& label, QWidget* field) {
    auto* labelWidget = new QLabel(label);
    labelWidget->setMinimumWidth(kFormLabelWidth);
    form->addRow(labelWidget, field);
}

QLabel* MakeMessageLabel(const QColor& color) {
    auto* label = new QLabel();
    label->setWordWrap(true);
    label->setStyleSheet(gui_theme::TextColorStyle(color));
    return label;
}

// The settings page scrolls. Without this, a wheel turn that passes over a spin
// box or combo box silently changes its value instead of scrolling the page.
class WheelGuard : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::Wheel) return false;
        auto* widget = qobject_cast<QWidget*>(watched);
        if (!widget || widget->hasFocus()) return false;
        // Ignored and filtered: the event goes on to the scroll area instead.
        event->ignore();
        return true;
    }
};

WheelGuard* wheelGuard = nullptr;

template <typename Widget>
Widget* GuardWheel(Widget* widget) {
    widget->setFocusPolicy(Qt::StrongFocus);
    if (wheelGuard) widget->installEventFilter(wheelGuard);
    return widget;
}

QComboBox* MakeComboBox() { return GuardWheel(new QComboBox()); }

QSpinBox* MakeSpinBox(int minimum, int maximum, int value) {
    auto* spin = GuardWheel(new QSpinBox());
    spin->setRange(minimum, maximum);
    spin->setValue(std::clamp(value, minimum, maximum));
    // Commit on Enter, focus loss or the arrows rather than on every digit, so
    // dependent clamps (peer timeout vs keepalive) do not fight the typing.
    spin->setKeyboardTracking(false);
    return spin;
}
}  // namespace

QLineEdit* MainWindow::AddTextField(QFormLayout* form, const QString& label,
                                    std::string* target, bool password) {
    auto* edit = new QLineEdit(QString::fromStdString(*target));
    if (password) edit->setEchoMode(QLineEdit::Password);
    connect(edit, &QLineEdit::textEdited, this, [this, target](const QString& text) {
        *target = text.toStdString();
        ScheduleConfigSave();
    });
    AddRow(form, label, edit);
    return edit;
}

QSpinBox* MainWindow::AddIntField(QFormLayout* form, const QString& label, int* target,
                                  int minimum, int maximum,
                                  std::function<void()> onChanged) {
    auto* spin = MakeSpinBox(minimum, maximum, *target);
    *target = spin->value();
    connect(spin, &QSpinBox::valueChanged, this,
            [this, target, onChanged = std::move(onChanged)](int value) {
                *target = value;
                if (onChanged) onChanged();
                ScheduleConfigSave();
            });
    AddRow(form, label, spin);
    return spin;
}

QCheckBox* MainWindow::AddCheckField(QFormLayout* form, const QString& label,
                                     bool* target, std::function<void()> onChanged) {
    auto* check = new QCheckBox();
    check->setChecked(*target);
    connect(check, &QCheckBox::toggled, this,
            [this, target, onChanged = std::move(onChanged)](bool checked) {
                *target = checked;
                if (onChanged) onChanged();
                ScheduleConfigSave();
            });
    AddRow(form, label, check);
    return check;
}

// Two balanced columns: session and timing settings on the left, the data path
// on the right. Every accepted edit is saved through the shared config module.
QWidget* MainWindow::BuildSettingsTab() {
    wheelGuard = new WheelGuard(this);
    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(kConfigSaveDelayMs);
    connect(saveTimer_, &QTimer::timeout, this, [this] { SaveGuiConfig(); });
    saveMessageTimer_ = new QTimer(this);
    saveMessageTimer_->setSingleShot(true);
    connect(saveMessageTimer_, &QTimer::timeout, this, [this] { configSaveLabel_->clear(); });

    auto* page = new QWidget();
    auto* pageLayout = new QVBoxLayout(page);
    auto* columns = new QHBoxLayout();
    auto* left = new QVBoxLayout();
    auto* right = new QVBoxLayout();
    columns->addLayout(left, 1);
    columns->addSpacing(8);
    columns->addLayout(right, 1);
    pageLayout->addLayout(columns);

    // ---- Rendezvous ----
    QFormLayout* form = AddSection(left, QStringLiteral("Rendezvous"));
    auto identityChanged = [this](QLineEdit* edit) {
        connect(edit, &QLineEdit::textEdited, this, [this] { UpdateIdentityLabel(); });
        rendezvousIdentityFields_.push_back(edit);
    };
    rendezvousIdentityFields_.push_back(
        AddTextField(form, QStringLiteral("Server address"), &config_.rendezvousAddress));
    rendezvousIdentityFields_.push_back(AddIntField(
        form, QStringLiteral("Server port"), &config_.rendezvousPort, 1, 65535));
    identityChanged(AddTextField(form, QStringLiteral("Room ID"), &config_.roomId));
    identityChanged(AddTextField(form, QStringLiteral("My peer ID"), &config_.peerId));
    rendezvousIdentityFields_.push_back(
        AddTextField(form, QStringLiteral("Auth token"), &config_.authToken, true));
    AddIntField(form, QStringLiteral("Retry delay (s)"),
                &config_.rendezvousRetryDelaySeconds, 1, 3600, [this] {
                    autoWaitRetryDelaySecondsRuntime_.store(
                        config_.rendezvousRetryDelaySeconds);
                });
    autoWaitCheck_ = AddCheckField(form, QStringLiteral("Auto wait for peer"),
                                   &config_.autoWaitForPeer,
                                   [this] { OnAutoWaitChanged(); });

    // ---- NAT liveness ----
    form = AddSection(left, QStringLiteral("NAT liveness"));
    config_.peerTimeout = std::max(config_.peerTimeout, config_.keepaliveInterval + 1);
    QSpinBox* keepalive = AddIntField(form, QStringLiteral("Keepalive (s)"),
                                      &config_.keepaliveInterval, 1, 300);
    QSpinBox* peerTimeout = AddIntField(form, QStringLiteral("Peer timeout (s)"),
        &config_.peerTimeout, config_.keepaliveInterval + 1, 3600);
    // Raising the minimum clamps the value, which saves it through valueChanged.
    connect(keepalive, &QSpinBox::valueChanged, peerTimeout,
            [peerTimeout](int value) { peerTimeout->setMinimum(value + 1); });

    // ---- NAT punch ----
    form = AddSection(left, QStringLiteral("NAT Punch"));
    if (config_.stunServers.size() < 2) config_.stunServers.resize(2);
    AddIntField(form, QStringLiteral("Punch timeout (s)"), &config_.punchTimeout, 1, 600);
    AddIntField(form, QStringLiteral("Attempt limit"), &config_.natPunchAttemptLimit, 1, 10);
    auto* profile = MakeComboBox();
    profile->addItem(QString::fromLatin1(
        NatPunchProfileDisplayName(NatPunchProfile::Balanced)));
    profile->addItem(QString::fromLatin1(
        NatPunchProfileDisplayName(NatPunchProfile::Aggressive)));
    profile->setCurrentIndex(std::clamp(static_cast<int>(config_.natPunchProfile), 0, 1));
    connect(profile, &QComboBox::currentIndexChanged, this, [this](int index) {
        config_.natPunchProfile = static_cast<NatPunchProfile>(std::clamp(index, 0, 1));
        ScheduleConfigSave();
    });
    AddRow(form, QStringLiteral("Profile"), profile);
    const char* stunNames[] = {"STUN A", "STUN B"};
    for (std::size_t index = 0; index < 2; ++index) {
        const QString name = QString::fromLatin1(stunNames[index]);
        AddTextField(form, name + QStringLiteral(" host"), &config_.stunServers[index].host);
        auto* port = MakeSpinBox(1, 65535, config_.stunServers[index].port);
        config_.stunServers[index].port = static_cast<uint16_t>(port->value());
        connect(port, &QSpinBox::valueChanged, this, [this, index](int value) {
            config_.stunServers[index].port = static_cast<uint16_t>(value);
            ScheduleConfigSave();
        });
        AddRow(form, name + QStringLiteral(" port"), port);
    }
    stunTestButton_ = new QPushButton(QStringLiteral("Test STUN A/B"));
    connect(stunTestButton_, &QPushButton::clicked, this, [this] { StartStunDiagnostic(); });
    auto* stunButtonRow = new QWidget();
    auto* stunButtonLayout = new QHBoxLayout(stunButtonRow);
    stunButtonLayout->setContentsMargins(0, 0, 0, 0);
    stunButtonLayout->addWidget(stunTestButton_);
    stunButtonLayout->addStretch(1);
    AddRow(form, QStringLiteral("STUN diagnostic"), stunButtonRow);
    stunResultLabel_ = MakeMessageLabel(gui_theme::kWarning);
    form->addRow(QString(), stunResultLabel_);
    UpdateStunDiagnosticUi();

    // ---- Log and misc ----
    form = AddSection(left, QStringLiteral("Log and misc"));
    auto* logLevel = MakeComboBox();
    for (const char* level : kLogLevels) logLevel->addItem(QString::fromLatin1(level));
    logLevel->setCurrentIndex(std::clamp(config_.logLevel, 0, 3));
    connect(logLevel, &QComboBox::currentIndexChanged, this, [this](int index) {
        config_.logLevel = std::clamp(index, 0, 3);
        ScheduleConfigSave();
    });
    AddRow(form, QStringLiteral("Log level"), logLevel);
    AddCheckField(form, QStringLiteral("1 KiB/s dummy traffic"),
                  &config_.dummyTrafficEnabled);
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        AddCheckField(form, QStringLiteral("Close to minimize"), &config_.closeToMinimize);
    }
#ifdef _WIN32
    bool startWithWindows = false;
    std::string startupError;
    if (!IsWindowsStartupEnabled(&startWithWindows, &startupError)) {
        Log(LogLevel::Error, startupError);
    }
    startWithWindowsCheck_ = new QCheckBox();
    startWithWindowsCheck_->setChecked(startWithWindows);
    startWithWindowsCheck_->setToolTip(
        QStringLiteral("Uses an elevated logon task in Windows Task Scheduler."));
    connect(startWithWindowsCheck_, &QCheckBox::toggled, this, [this](bool enabled) {
        std::string error;
        if (SetWindowsStartupEnabled(enabled, &error)) {
            ShowConfigSaveMessage(enabled ? "Windows startup enabled"
                                          : "Windows startup disabled",
                                  true);
            return;
        }
        const QSignalBlocker blocker(startWithWindowsCheck_);
        startWithWindowsCheck_->setChecked(!enabled);
        ShowConfigSaveMessage(error, false);
        Log(LogLevel::Error, error);
    });
    AddRow(form, QStringLiteral("Start with Windows"), startWithWindowsCheck_);
#endif
    left->addStretch(1);

    // ---- TUN adapter ----
    form = AddSection(right, QStringLiteral("TUN adapter"));
    AddTextField(form, QStringLiteral("Adapter name"), &config_.adapterName);
    AddTextField(form, QStringLiteral("Local TUN IPv4"), &config_.localTunIpv4);
    AddIntField(form, QStringLiteral("TUN prefix"), &config_.tunPrefix, 0, 32);
    mtuWarning_ = MakeMessageLabel(QColor(255, 204, 0));
    mtuWarning_->setText(QStringLiteral("MTU > 1472 may cause outer IPv4 fragmentation"));
    AddIntField(form, QStringLiteral("TUN MTU"), &config_.tunMtu, 576, 9000,
                [this] { mtuWarning_->setVisible(config_.tunMtu > 1472); });
    form->addRow(QString(), mtuWarning_);
    mtuWarning_->setVisible(config_.tunMtu > 1472);
    AddCheckField(form, QStringLiteral("Auto configure IPv4"), &config_.autoConfigIpv4);

    // ---- Traversal strategy ----
    auto* traversalBox = new QGroupBox(QStringLiteral("Traversal strategy"));
    auto* traversalLayout = new QVBoxLayout(traversalBox);
    traversalTable_ = new QTableWidget(0, 4, traversalBox);
    traversalTable_->setHorizontalHeaderLabels({QStringLiteral("Enabled"),
        QStringLiteral("Priority"), QStringLiteral("Mode"), QStringLiteral("Order")});
    traversalTable_->verticalHeader()->setVisible(false);
    traversalTable_->verticalHeader()->setDefaultSectionSize(34);
    traversalTable_->setShowGrid(false);
    traversalTable_->setSelectionMode(QAbstractItemView::NoSelection);
    traversalTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    traversalTable_->setFocusPolicy(Qt::NoFocus);
    QHeaderView* traversalHeader = traversalTable_->horizontalHeader();
    traversalHeader->setSectionResizeMode(QHeaderView::ResizeToContents);
    traversalHeader->setSectionResizeMode(2, QHeaderView::Stretch);
    // Cell widgets do not feed ResizeToContents; size the Up/Down column here.
    traversalHeader->setSectionResizeMode(3, QHeaderView::Fixed);
    traversalHeader->resizeSection(3, 130);
    traversalTable_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    traversalLayout->addWidget(traversalTable_);
    right->addWidget(traversalBox);
    RebuildTraversalTable();

    // ---- IPv6 direct connection ----
    form = AddSection(right, QStringLiteral("IPv6 direct connection"));
    AddCheckField(form, QStringLiteral("Accept inbound UDP"), &config_.ipv6AcceptInbound);
    AddIntField(form, QStringLiteral("Listen port (0=auto)"), &config_.ipv6ListenPort,
                0, 65535);
    AddTextField(form, QStringLiteral("Probe host"), &config_.ipv6ProbeHost);
    AddIntField(form, QStringLiteral("Probe TCP port"), &config_.ipv6ProbePort, 1, 65535);
    AddIntField(form, QStringLiteral("Probe timeout (s)"), &config_.ipv6FallbackTimeout,
                1, 120);
    right->addStretch(1);

    configSaveLabel_ = new QLabel();
    configSaveLabel_->setWordWrap(true);
    pageLayout->addWidget(configSaveLabel_);
    pageLayout->addStretch(1);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(page);
    return scroll;
}

void MainWindow::RebuildTraversalTable() {
    const int count = static_cast<int>(config_.traversalModes.size());
    traversalTable_->setRowCount(count);
    for (int row = 0; row < count; ++row) {
        const TraversalModeSetting& setting = config_.traversalModes[row];
        // A real check box: item-view indicators vanish on the dark palette.
        auto* enabledCell = new QWidget();
        auto* enabledLayout = new QHBoxLayout(enabledCell);
        enabledLayout->setContentsMargins(0, 0, 0, 0);
        enabledLayout->setAlignment(Qt::AlignCenter);
        auto* enabled = new QCheckBox(enabledCell);
        enabled->setChecked(setting.enabled);
        enabledLayout->addWidget(enabled);
        connect(enabled, &QCheckBox::toggled, this, [this, row](bool checked) {
            config_.traversalModes[row].enabled = checked;
            ScheduleConfigSave();
        });
        traversalTable_->setCellWidget(row, 0, enabledCell);
        auto* priority = new QTableWidgetItem(QString::number(row + 1));
        priority->setFlags(Qt::ItemIsEnabled);
        priority->setTextAlignment(Qt::AlignCenter);
        traversalTable_->setItem(row, 1, priority);
        auto* mode = new QTableWidgetItem(
            QString::fromLatin1(TraversalModeDisplayName(setting.mode)));
        mode->setFlags(Qt::ItemIsEnabled);
        traversalTable_->setItem(row, 2, mode);

        auto* order = new QWidget();
        auto* orderLayout = new QHBoxLayout(order);
        orderLayout->setContentsMargins(2, 0, 2, 0);
        orderLayout->setSpacing(2);
        auto* up = new QPushButton(QStringLiteral("Up"), order);
        auto* down = new QPushButton(QStringLiteral("Down"), order);
        up->setEnabled(row > 0);
        down->setEnabled(row + 1 < count);
        for (QPushButton* button : {up, down}) {
            gui_theme::SetButtonVariant(button, "compact");
            orderLayout->addWidget(button);
        }
        auto move = [this, row](int offset) {
            // Deferred: the clicked button is destroyed by the rebuild.
            QTimer::singleShot(0, this, [this, row, offset] {
                std::swap(config_.traversalModes[row], config_.traversalModes[row + offset]);
                RebuildTraversalTable();
                ScheduleConfigSave();
            });
        };
        connect(up, &QPushButton::clicked, this, [move] { move(-1); });
        connect(down, &QPushButton::clicked, this, [move] { move(1); });
        traversalTable_->setCellWidget(row, 3, order);
    }
    traversalTable_->setFixedHeight(traversalTable_->horizontalHeader()->sizeHint().height()
        + traversalTable_->verticalHeader()->defaultSectionSize() * std::max(count, 1)
        + traversalTable_->frameWidth() * 2);
}

void MainWindow::OnAutoWaitChanged() {
    autoWaitEnabledRuntime_.store(config_.autoWaitForPeer);
    if (config_.autoWaitForPeer) {
        autoWaitRetryAfterMs_.store(0);
        const TunnelState state = currentState_.load();
        if (state == TunnelState::Disconnected || state == TunnelState::Error) {
            autoWaitPending_.store(true);
        }
    } else {
        autoWaitPending_.store(false);
    }
}

void MainWindow::StartStunDiagnostic() {
    if (stunDiagnosticRunning_.exchange(true)) return;
    if (stunDiagnosticThread_.joinable()) stunDiagnosticThread_.join();

    const std::vector<StunServerConfig> servers = config_.stunServers;
    {
        std::lock_guard<std::mutex> lock(stunDiagnosticMutex_);
        stunDiagnosticMessage_ = "Testing both STUN servers with one UDP socket...";
        stunDiagnosticCompleted_ = false;
        stunDiagnosticSucceeded_ = false;
    }
    UpdateStunDiagnosticUi();
    stunDiagnosticThread_ = std::thread([this, servers] {
        StunDiagnosticResult result;
        std::string error;
        const bool succeeded = DiagnoseStunServers(servers, 800, 3, &result, &error);
        const std::string message = succeeded
            ? FormatStunDiagnosticSummary(result)
            : "STUN diagnostic failed: " + error;
        {
            std::lock_guard<std::mutex> lock(stunDiagnosticMutex_);
            stunDiagnosticMessage_ = message;
            stunDiagnosticCompleted_ = true;
            stunDiagnosticSucceeded_ = succeeded;
        }
        stunDiagnosticRunning_.store(false);
        stunDiagnosticDirty_.store(true);
        Log(succeeded ? LogLevel::Info : LogLevel::Error, message);
    });
}

void MainWindow::UpdateStunDiagnosticUi() {
    const bool running = stunDiagnosticRunning_.load();
    stunTestButton_->setEnabled(!running);
    stunTestButton_->setText(running ? QStringLiteral("Testing...")
                                     : QStringLiteral("Test STUN A/B"));
    std::lock_guard<std::mutex> lock(stunDiagnosticMutex_);
    const QColor color = stunDiagnosticSucceeded_ ? gui_theme::kSuccess
        : stunDiagnosticCompleted_ ? gui_theme::kFailure : gui_theme::kWarning;
    stunResultLabel_->setStyleSheet(gui_theme::TextColorStyle(color));
    stunResultLabel_->setText(QString::fromStdString(stunDiagnosticMessage_));
}

void MainWindow::JoinStunDiagnostic() {
    if (stunDiagnosticThread_.joinable()) stunDiagnosticThread_.join();
}

std::string MainWindow::LoadGuiConfig(bool* succeeded) {
    bool existed = false;
    std::string error;
    if (!LoadClientConfig(configFilePath_, &config_, &existed, &error)) {
        Log(LogLevel::Error, error);
        *succeeded = false;
        return error;
    }
    const std::string message = existed
        ? "Configuration loaded from " + configFilePath_
        : "Configuration will be saved to " + configFilePath_;
    if (existed) Log(LogLevel::Info, message);
    *succeeded = true;
    return message;
}

bool MainWindow::SaveGuiConfig() {
    std::string error;
    if (!SaveClientConfig(configFilePath_, config_, &error)) {
        ShowConfigSaveMessage(error, false);
        Log(LogLevel::Error, error);
        return false;
    }
    ShowConfigSaveMessage("Configuration saved: " + configFilePath_, true);
    return true;
}

// Edits arrive per keystroke; coalesce them into one write.
void MainWindow::ScheduleConfigSave() { saveTimer_->start(); }

void MainWindow::ShowConfigSaveMessage(const std::string& message, bool succeeded) {
    configSaveLabel_->setStyleSheet(gui_theme::TextColorStyle(
        succeeded ? gui_theme::kSuccess : gui_theme::kFailure));
    configSaveLabel_->setText(QString::fromStdString(message));
    saveMessageTimer_->start(kConfigSaveMessageMs);
}
