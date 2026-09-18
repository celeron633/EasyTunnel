#include "main_window.h"

#include <algorithm>
#include <utility>

#include <QApplication>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "gui_theme.h"
#include "history_chart.h"

namespace {
constexpr auto kActivityWindow = std::chrono::milliseconds(350);
constexpr int kMaxVisiblePeerRows = 6;

const char* ByteUnitName(int unit) {
    switch (unit) {
        case 1: return "KB";
        case 2: return "MB";
        default: return "Bytes";
    }
}

QString FormatBytes(double bytes, int unit, bool perSecond) {
    QString text;
    if (unit == 0) {
        text = QString::number(static_cast<unsigned long long>(bytes));
    } else {
        const double divisor = unit == 1 ? 1024.0 : 1024.0 * 1024.0;
        text = QString::number(bytes / divisor, 'f', 2);
    }
    text += ' ';
    text += QString::fromLatin1(ByteUnitName(unit));
    if (perSecond) text += QStringLiteral("/s");
    return text;
}

QString FormatIdleTime(uint64_t seconds) {
    if (seconds < 60) return QStringLiteral("%1s").arg(seconds);
    if (seconds < 3600) return QStringLiteral("%1m %2s").arg(seconds / 60).arg(seconds % 60);
    return QStringLiteral("%1h %2m").arg(seconds / 3600).arg((seconds % 3600) / 60);
}

// Value plus unit in one flat cell. It has to read as a number, not a button,
// so the frame stays transparent until the pointer is over it.
QPushButton* MakeUnitButton(QWidget* parent) {
    auto* button = new QPushButton(parent);
    button->setFlat(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(QStringLiteral("Click to switch unit"));
    button->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none; padding: 2px 6px;"
        " text-align: left; }"
        "QPushButton:hover { background: %1; border-radius: 4px; }")
        .arg(gui_theme::kSurfaceRaised.name()));
    return button;
}

QString DotStyle(bool active, const QColor& color) {
    return gui_theme::TextColorStyle(active ? color : gui_theme::kBorder.lighter(130));
}

// Empty cells show a muted placeholder when the server did not report a field.
QTableWidgetItem* PeerItem(const std::string& value) {
    auto* item = new QTableWidgetItem(
        value.empty() ? QStringLiteral("--") : QString::fromStdString(value));
    if (value.empty()) item->setForeground(gui_theme::kMuted);
    return item;
}
}  // namespace

QWidget* MainWindow::BuildConnectionTab() {
    auto* tab = new QWidget(this);
    auto* layout = new QVBoxLayout(tab);

    // Online peers: the whole action toolbar lives on one row above the list,
    // the way the terminal client lays it out.
    auto* peersBox = new QGroupBox(QStringLiteral("Online peers"), tab);
    auto* peersLayout = new QVBoxLayout(peersBox);
    auto* toolbar = new QHBoxLayout();
    refreshButton_ = new QPushButton(QStringLiteral("Refresh"), peersBox);
    waitButton_ = new QPushButton(QStringLiteral("Wait for peer"), peersBox);
    connectButton_ = new QPushButton(QStringLiteral("Connect selected"), peersBox);
    refreshButton_->setMinimumWidth(90);
    waitButton_->setMinimumWidth(120);
    connectButton_->setMinimumWidth(150);
    gui_theme::SetButtonVariant(waitButton_, "primary");
    gui_theme::SetButtonVariant(connectButton_, "primary");
    onlineLabel_ = new QLabel(peersBox);
    onlineLabel_->setStyleSheet(gui_theme::TextColorStyle(gui_theme::kMuted));
    toolbar->addWidget(refreshButton_);
    toolbar->addWidget(waitButton_);
    toolbar->addWidget(connectButton_);
    toolbar->addStretch(1);
    toolbar->addWidget(onlineLabel_);
    peersLayout->addLayout(toolbar);

    peerTable_ = new QTableWidget(0, 5, peersBox);
    peerTable_->setHorizontalHeaderLabels({QStringLiteral("Peer ID"),
        QStringLiteral("Public endpoint"), QStringLiteral("Capabilities"),
        QStringLiteral("TUN IP"), QStringLiteral("Idle")});
    peerTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    peerTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    peerTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    peerTable_->setAlternatingRowColors(true);
    peerTable_->verticalHeader()->setVisible(false);
    peerTable_->verticalHeader()->setDefaultSectionSize(30);
    peerTable_->setShowGrid(false);
    peerTable_->setFocusPolicy(Qt::NoFocus);
    peerTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    peerTable_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    peersLayout->addWidget(peerTable_);
    layout->addWidget(peersBox);

    connect(refreshButton_, &QPushButton::clicked, this, [this] { RefreshClients(); });
    connect(waitButton_, &QPushButton::clicked, this, [this] {
        if (IsTunnelActive(currentState_.load())) Disconnect();
        else StartConnection("");
        stateDirty_.store(true);
    });
    connect(connectButton_, &QPushButton::clicked, this, [this] { ConnectSelectedClient(); });
    connect(peerTable_, &QTableWidget::itemSelectionChanged, this,
            [this] { RefreshStateUi(); });

    // Traffic: one row per direction keeps packets, totals and speed under
    // shared headers instead of repeating the labels six times.
    auto* trafficBox = new QGroupBox(QStringLiteral("Traffic"), tab);
    auto* trafficLayout = new QVBoxLayout(trafficBox);
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(16);
    const char* headers[] = {"", "Packets", "Total", "Speed"};
    for (int column = 0; column < 4; ++column) {
        auto* header = new QLabel(QString::fromLatin1(headers[column]), trafficBox);
        header->setStyleSheet(gui_theme::TextColorStyle(gui_theme::kMuted));
        grid->addWidget(header, 0, column);
    }
    auto addRow = [&](int row, const char* name, TrafficRow* target) {
        auto* nameCell = new QWidget(trafficBox);
        auto* nameLayout = new QHBoxLayout(nameCell);
        nameLayout->setContentsMargins(0, 0, 0, 0);
        nameLayout->setSpacing(6);
        target->dot = new QLabel(QString(QChar(0x25CF)), nameCell);
        nameLayout->addWidget(target->dot);
        nameLayout->addWidget(new QLabel(QString::fromLatin1(name), nameCell));
        nameLayout->addStretch(1);
        grid->addWidget(nameCell, row, 0);
        target->packets = new QLabel(trafficBox);
        grid->addWidget(target->packets, row, 1);
        target->total = MakeUnitButton(trafficBox);
        grid->addWidget(target->total, row, 2, Qt::AlignLeft);
        target->speed = MakeUnitButton(trafficBox);
        grid->addWidget(target->speed, row, 3, Qt::AlignLeft);
        connect(target->total, &QPushButton::clicked, this, [this] {
            statisticsTotalUnit_ = (statisticsTotalUnit_ + 1) % 3;
            UpdateTrafficUi();
        });
        connect(target->speed, &QPushButton::clicked, this, [this] {
            statisticsSpeedUnit_ = (statisticsSpeedUnit_ + 1) % 3;
            UpdateTrafficUi();
        });
    };
    addRow(1, "TX", &txRow_);
    addRow(2, "RX", &rxRow_);
    grid->setColumnMinimumWidth(0, 70);
    for (int column = 1; column < 4; ++column) grid->setColumnStretch(column, 1);
    trafficLayout->addLayout(grid);
    latencyLabel_ = new QLabel(trafficBox);
    trafficLayout->addWidget(latencyLabel_);
    layout->addWidget(trafficBox);

    auto* historyBox = new QGroupBox(QStringLiteral("Last 60 seconds"), tab);
    auto* historyLayout = new QHBoxLayout(historyBox);
    const int capacity = static_cast<int>(StatisticsHistory::kMaxSamples);
    charts_[0] = new HistoryChart(QStringLiteral("TX speed"), QStringLiteral("KiB/s"),
                                  gui_theme::kTx, capacity, historyBox);
    charts_[1] = new HistoryChart(QStringLiteral("RX speed"), QStringLiteral("KiB/s"),
                                  gui_theme::kRx, capacity, historyBox);
    charts_[2] = new HistoryChart(QStringLiteral("Latency"), QStringLiteral("ms"),
                                  gui_theme::kLatency, capacity, historyBox);
    for (HistoryChart* chart : charts_) historyLayout->addWidget(chart);
    layout->addWidget(historyBox, 1);

    RebuildPeerTable();
    return tab;
}

// The list grows with the room instead of always reserving space for six
// peers, so an empty room leaves the space to the charts below.
void MainWindow::RebuildPeerTable() {
    peerTable_->clearContents();
    peerTable_->clearSpans();
    if (clients_.empty()) {
        peerTable_->setRowCount(1);
        auto* placeholder = new QTableWidgetItem(
            QStringLiteral("No online peers. Press Refresh."));
        placeholder->setForeground(gui_theme::kMuted);
        placeholder->setFlags(Qt::ItemIsEnabled);
        peerTable_->setItem(0, 0, placeholder);
        peerTable_->setSpan(0, 0, 1, peerTable_->columnCount());
    } else {
        peerTable_->setRowCount(static_cast<int>(clients_.size()));
        for (int row = 0; row < static_cast<int>(clients_.size()); ++row) {
            const RendezvousPeerInfo& client = clients_[row];
            peerTable_->setItem(row, 0, PeerItem(client.peerId));
            peerTable_->setItem(row, 1, PeerItem(client.endpoint));
            QTableWidgetItem* capabilities =
                PeerItem(SerializeTraversalModeSequence(client.capabilities));
            capabilities->setToolTip(
                QString::fromStdString(FormatPeerCapabilities(client.capabilities)));
            peerTable_->setItem(row, 2, capabilities);
            peerTable_->setItem(row, 3, PeerItem(client.tunIp));
            auto* idle = new QTableWidgetItem(FormatIdleTime(client.idleSeconds));
            peerTable_->setItem(row, 4, idle);
        }
        peerTable_->selectRow(0);
    }
    const int visibleRows = std::clamp(peerTable_->rowCount(), 1, kMaxVisiblePeerRows);
    const int rowHeight = peerTable_->verticalHeader()->defaultSectionSize();
    peerTable_->setFixedHeight(peerTable_->horizontalHeader()->sizeHint().height()
        + rowHeight * visibleRows + peerTable_->frameWidth() * 2);
    onlineLabel_->setText(QStringLiteral("%1 online").arg(clients_.size()));
    RefreshStateUi();
}

void MainWindow::UpdateTrafficUi() {
    const auto& stats = engine_.GetStats();
    const auto now = std::chrono::steady_clock::now();
    auto fill = [this, now](TrafficRow& row, const QColor& color,
                            std::chrono::steady_clock::time_point lastActivity,
                            uint64_t packets, uint64_t bytes, double bytesPerSecond) {
        // Re-polishing a style sheet is not free; only touch it on a change.
        const QString dotStyle = DotStyle(now - lastActivity < kActivityWindow, color);
        if (row.dot->styleSheet() != dotStyle) row.dot->setStyleSheet(dotStyle);
        row.packets->setText(QString::number(packets));
        row.total->setText(FormatBytes(static_cast<double>(bytes),
                                       statisticsTotalUnit_, false));
        row.speed->setText(FormatBytes(bytesPerSecond, statisticsSpeedUnit_, true));
    };
    fill(txRow_, gui_theme::kTx, lastTxActivity_, stats.txPackets.load(),
         stats.txBytes.load(), txBytesPerSecond_);
    fill(rxRow_, gui_theme::kRx, lastRxActivity_, stats.rxPackets.load(),
         stats.rxBytes.load(), rxBytesPerSecond_);

    const int64_t rtt = stats.rttMilliseconds.load();
    const QString latency = rtt < 0 ? QStringLiteral("--") : QString::number(rtt);
    latencyLabel_->setText(QStringLiteral("Latency  %1 ms      TUN ring drops  %2")
        .arg(latency)
        .arg(stats.tunRingFullDrops.load()));
}

void MainWindow::UpdateCharts() {
    const auto& samples = statisticsHistory_.Samples();
    const auto newest = samples.empty()
        ? std::chrono::system_clock::time_point{} : samples.back().timestamp;
    if (samples.size() == renderedSampleCount_ && newest == renderedSampleTime_) return;
    renderedSampleCount_ = samples.size();
    renderedSampleTime_ = newest;

    std::vector<std::chrono::system_clock::time_point> times;
    std::array<std::vector<double>, 3> values;
    times.reserve(samples.size());
    for (auto& series : values) series.reserve(samples.size());
    for (const StatisticsSample& sample : samples) {
        times.push_back(sample.timestamp);
        values[0].push_back(sample.txKibPerSecond);
        values[1].push_back(sample.rxKibPerSecond);
        values[2].push_back(sample.latencyMilliseconds);
    }
    for (std::size_t index = 0; index < charts_.size(); ++index) {
        charts_[index]->SetData(std::move(values[index]), times);
    }
}

void MainWindow::UpdateLiveStats() {
    const auto& stats = engine_.GetStats();
    const auto now = std::chrono::steady_clock::now();
    const uint64_t txPackets = stats.txPackets.load();
    const uint64_t rxPackets = stats.rxPackets.load();
    if (txPackets > observedTxPackets_) lastTxActivity_ = now;
    if (rxPackets > observedRxPackets_) lastRxActivity_ = now;
    observedTxPackets_ = txPackets;
    observedRxPackets_ = rxPackets;

    const bool connected = currentState_.load() == TunnelState::Connected;
    UpdateTray(connected && now - lastRxActivity_ < kActivityWindow,
               connected && now - lastTxActivity_ < kActivityWindow);

    const uint64_t txBytes = stats.txBytes.load();
    const uint64_t rxBytes = stats.rxBytes.load();
    if (!speedSampleInitialized_) {
        previousTxBytes_ = txBytes;
        previousRxBytes_ = rxBytes;
        lastSpeedSample_ = now;
        speedSampleInitialized_ = true;
        return;
    }
    const double elapsed = std::chrono::duration<double>(now - lastSpeedSample_).count();
    if (elapsed < 1.0) return;
    const uint64_t txDelta = txBytes >= previousTxBytes_ ? txBytes - previousTxBytes_ : 0;
    const uint64_t rxDelta = rxBytes >= previousRxBytes_ ? rxBytes - previousRxBytes_ : 0;
    txBytesPerSecond_ = static_cast<double>(txDelta) / elapsed;
    rxBytesPerSecond_ = static_cast<double>(rxDelta) / elapsed;
    previousTxBytes_ = txBytes;
    previousRxBytes_ = rxBytes;
    lastSpeedSample_ = now;
}

bool MainWindow::StartConnection(const std::string& targetPeerId) {
    std::string error;
    if (!ValidateClientConfig(config_, &error)) {
        OnStateChanged(TunnelState::Error, error);
        return false;
    }
    if (!targetPeerId.empty() && targetPeerId == config_.peerId) {
        OnStateChanged(TunnelState::Error, "Cannot connect to this client itself");
        return false;
    }
    const bool started = engine_.Start(ToEngineConfig(config_, targetPeerId));
    if (started) waitingForPeer_.store(targetPeerId.empty());
    stateDirty_.store(true);
    return started;
}

void MainWindow::ConnectSelectedClient() {
    const int row = peerTable_->currentRow();
    if (row < 0 || row >= static_cast<int>(clients_.size())) return;
    const std::string target = clients_[row].peerId;
    suppressAutoWait_.store(true);
    autoWaitPending_.store(false);
    engine_.Stop();
    waitingForPeer_.store(false);
    suppressAutoWait_.store(false);
    if (!StartConnection(target) && autoWaitEnabledRuntime_.load()) {
        autoWaitPending_.store(true);
    }
}

void MainWindow::RefreshClients() {
    std::string error;
    if (!ValidateClientConfig(config_, &error)) {
        SetStatusMessage(error);
        return;
    }
    std::vector<RendezvousPeerInfo> clients;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool listed = ListRendezvousClients(
        config_.rendezvousAddress, static_cast<uint16_t>(config_.rendezvousPort),
        config_.roomId, config_.authToken, &clients, &error);
    QApplication::restoreOverrideCursor();
    if (!listed) {
        SetStatusMessage(error);
        return;
    }
    clients.erase(std::remove_if(clients.begin(), clients.end(),
                                 [this](const RendezvousPeerInfo& client) {
                                     return client.peerId == config_.peerId;
                                 }),
                  clients.end());
    std::sort(clients.begin(), clients.end(),
              [](const RendezvousPeerInfo& left, const RendezvousPeerInfo& right) {
                  return left.peerId < right.peerId;
              });
    clients_ = std::move(clients);
    RebuildPeerTable();
    SetStatusMessage(clients_.empty() ? "No online clients" : "Client list refreshed");
}

void MainWindow::Disconnect() {
    engine_.Stop();
    waitingForPeer_.store(false);
    OnStateChanged(TunnelState::Disconnected, "Disconnected");
}

void MainWindow::ProcessAutoWait() {
    if (shuttingDown_.load() || !autoWaitEnabledRuntime_.load()
        || !autoWaitPending_.load()) return;
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (nowMs < autoWaitRetryAfterMs_.load()) return;
    const TunnelState state = currentState_.load();
    if (state != TunnelState::Disconnected && state != TunnelState::Error) return;
    std::string error;
    if (!ValidateClientConfig(config_, &error)) {
        OnStateChanged(TunnelState::Error, "Auto wait failed: " + error);
        autoWaitPending_.store(false);
        return;
    }
    if (StartConnection("")) {
        autoWaitPending_.store(false);
    } else {
        autoWaitRetryAfterMs_.store(nowMs
            + static_cast<int64_t>(autoWaitRetryDelaySecondsRuntime_.load()) * 1000);
    }
}

// May run on an engine worker thread; the next tick repaints the state.
void MainWindow::OnStateChanged(TunnelState state, const std::string& message) {
    currentState_.store(state);
    if (state == TunnelState::Connected) waitingForPeer_.store(false);
    if (state == TunnelState::Disconnected || state == TunnelState::Error) {
        waitingForPeer_.store(false);
        const int64_t retryAt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()
            + static_cast<int64_t>(autoWaitRetryDelaySecondsRuntime_.load()) * 1000;
        autoWaitRetryAfterMs_.store(retryAt);
        if (!shuttingDown_.load() && !suppressAutoWait_.load()
            && autoWaitEnabledRuntime_.load()) {
            autoWaitPending_.store(true);
        }
    }
    SetStatusMessage(message.empty() ? "Disconnected" : message);
}

void MainWindow::SetStatusMessage(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        statusMessage_ = message;
    }
    stateDirty_.store(true);
}
