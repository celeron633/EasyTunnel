#include "main_window.h"

#include <algorithm>
#include <utility>

#include <QApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
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

enum TrafficColumn { kDirectionColumn, kPacketsColumn, kTotalColumn, kSpeedColumn };
enum TrafficRowIndex { kTxRow, kRxRow };

// Activity dot for a traffic row: full colour while packets move, a neutral
// grey otherwise. Drawn at 2x so it stays crisp on high-DPI screens.
QIcon ActivityIcon(const QColor& color, bool active) {
    QPixmap pixmap(24, 24);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(active ? color : gui_theme::kOutlineVariant);
    painter.drawEllipse(QRectF(1.0, 1.0, 10.0, 10.0));
    return QIcon(pixmap);
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
    peerTable_->verticalHeader()->setVisible(false);
    peerTable_->verticalHeader()->setDefaultSectionSize(40);
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
    // shared headers. Clicking a Total or Speed cell (or its header) cycles
    // the unit of that column.
    auto* trafficBox = new QGroupBox(QStringLiteral("Traffic"), tab);
    auto* trafficLayout = new QVBoxLayout(trafficBox);
    trafficTable_ = new QTableWidget(2, 4, trafficBox);
    trafficTable_->setHorizontalHeaderLabels({QStringLiteral("Direction"),
        QStringLiteral("Packets"), QStringLiteral("Total"), QStringLiteral("Speed")});
    trafficTable_->setSelectionMode(QAbstractItemView::NoSelection);
    trafficTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    trafficTable_->setFocusPolicy(Qt::NoFocus);
    trafficTable_->setShowGrid(false);
    trafficTable_->setIconSize(QSize(12, 12));
    trafficTable_->verticalHeader()->setVisible(false);
    trafficTable_->verticalHeader()->setDefaultSectionSize(40);
    trafficTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    trafficTable_->horizontalHeader()->setHighlightSections(false);
    trafficTable_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    trafficTable_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    const QString unitHint = QStringLiteral("Click to switch unit");
    const char* directions[] = {"TX", "RX"};
    for (int row = 0; row < 2; ++row) {
        trafficTable_->setItem(row, kDirectionColumn,
                               new QTableWidgetItem(QString::fromLatin1(directions[row])));
        for (int column = kPacketsColumn; column <= kSpeedColumn; ++column) {
            auto* item = new QTableWidgetItem();
            if (column != kPacketsColumn) item->setToolTip(unitHint);
            trafficTable_->setItem(row, column, item);
        }
    }
    trafficTable_->horizontalHeaderItem(kTotalColumn)->setToolTip(unitHint);
    trafficTable_->horizontalHeaderItem(kSpeedColumn)->setToolTip(unitHint);
    trafficTable_->setFixedHeight(trafficTable_->horizontalHeader()->sizeHint().height()
        + trafficTable_->verticalHeader()->defaultSectionSize() * 2
        + trafficTable_->frameWidth() * 2);
    auto cycleUnit = [this](int column) {
        if (column == kTotalColumn) statisticsTotalUnit_ = (statisticsTotalUnit_ + 1) % 3;
        else if (column == kSpeedColumn) statisticsSpeedUnit_ = (statisticsSpeedUnit_ + 1) % 3;
        else return;
        UpdateTrafficUi();
    };
    connect(trafficTable_, &QTableWidget::cellClicked, this,
            [cycleUnit](int, int column) { cycleUnit(column); });
    connect(trafficTable_->horizontalHeader(), &QHeaderView::sectionClicked, this,
            cycleUnit);
    trafficLayout->addWidget(trafficTable_);
    latencyLabel_ = new QLabel(trafficBox);
    latencyLabel_->setStyleSheet(gui_theme::TextColorStyle(gui_theme::kMuted)
        + QStringLiteral(" padding: 4px 8px 0 8px;"));
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
    auto fill = [this, now](int row, const QColor& color,
                            std::chrono::steady_clock::time_point lastActivity,
                            uint64_t packets, uint64_t bytes, double bytesPerSecond) {
        // Icons are rebuilt only when the activity flips, not on every tick.
        const int active = now - lastActivity < kActivityWindow ? 1 : 0;
        if (trafficRowActive_[row] != active) {
            trafficRowActive_[row] = active;
            trafficTable_->item(row, kDirectionColumn)->setIcon(ActivityIcon(color, active));
        }
        trafficTable_->item(row, kPacketsColumn)->setText(QString::number(packets));
        trafficTable_->item(row, kTotalColumn)->setText(
            FormatBytes(static_cast<double>(bytes), statisticsTotalUnit_, false));
        trafficTable_->item(row, kSpeedColumn)->setText(
            FormatBytes(bytesPerSecond, statisticsSpeedUnit_, true));
    };
    fill(kTxRow, gui_theme::kTx, lastTxActivity_, stats.txPackets.load(),
         stats.txBytes.load(), txBytesPerSecond_);
    fill(kRxRow, gui_theme::kRx, lastRxActivity_, stats.rxPackets.load(),
         stats.rxBytes.load(), rxBytesPerSecond_);

    const int64_t rtt = stats.rttMilliseconds.load();
    const QString latency = rtt < 0 ? QStringLiteral("--") : QString::number(rtt);
    latencyLabel_->setText(QStringLiteral("Latency %1 ms   %2   TUN ring drops %3")
        .arg(latency)
        .arg(QChar(0x00B7))
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
