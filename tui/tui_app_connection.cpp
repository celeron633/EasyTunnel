#include "tui_app.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <utility>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "../rendezvous_client.h"
#include "tui_theme.h"
#ifdef _WIN32
#include "windows_tray.h"
#endif

namespace {
const char* UnitName(int unit) {
    if (unit == 1) return "KB";
    if (unit == 2) return "MB";
    return "Bytes";
}

std::string ByteText(double bytes, int unit, bool speed) {
    double value = bytes;
    if (unit == 1) value /= 1024.0;
    if (unit == 2) value /= 1024.0 * 1024.0;
    std::ostringstream output;
    if (unit == 0) output << static_cast<unsigned long long>(value);
    else output << std::fixed << std::setprecision(2) << value;
    output << ' ' << UnitName(unit);
    if (speed) output << "/s";
    return output.str();
}

int64_t SteadyMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr size_t kClientColumnCount = 5;
constexpr size_t kMaxClientColumnWidth = 28;
constexpr const char* kMissingField = "--";
const std::array<std::string, kClientColumnCount> kClientColumnTitles{
    "Peer ID", "Public endpoint", "Capabilities", "TUN IP", "Idle"};

struct ClientColumns {
    std::array<size_t, kClientColumnCount> widths{};

    ClientColumns() {
        for (size_t i = 0; i < kClientColumnCount; ++i) {
            widths[i] = kClientColumnTitles[i].size();
        }
    }
};

std::string JoinClientColumns(const ClientColumns& columns,
                              const std::array<std::string, kClientColumnCount>& row) {
    std::string line;
    for (size_t i = 0; i < kClientColumnCount; ++i) {
        if (i > 0) line += "  ";
        line += row[i];
        if (i + 1 == kClientColumnCount) break;
        if (row[i].size() < columns.widths[i]) {
            line.append(columns.widths[i] - row[i].size(), ' ');
        }
    }
    return line;
}

std::string PeerField(const std::string& value) {
    return value.empty() ? kMissingField : value;
}

std::string FormatIdleTime(uint64_t seconds) {
    if (seconds < 60) return std::to_string(seconds) + "s";
    if (seconds < 3600) {
        return std::to_string(seconds / 60) + "m" + std::to_string(seconds % 60) + "s";
    }
    return std::to_string(seconds / 3600) + "h"
        + std::to_string((seconds % 3600) / 60) + "m";
}
}  // namespace

ftxui::Component TuiApp::BuildConnectionTab() {
    using namespace ftxui;

    auto clientList = Radiobox(&clients_, &selectedClient_);
    auto refresh = Button("Refresh", [this] { RefreshClients(); },
                          tui_theme::FlatButton());
    auto wait = Button("Wait for peer", [this] { StartConnection(""); },
                       tui_theme::FlatButton());
    auto connect = Button("Connect selected", [this] { ConnectSelectedClient(); },
                          tui_theme::FlatButton());
    auto disconnect = Button("Disconnect", [this] { Disconnect(); },
                             tui_theme::FlatButton());
    const ButtonOption valueButton = tui_theme::ValueButton();
    auto txTotal = Button(&txTotalLabel_,
        [this] { txTotalUnit_ = (txTotalUnit_ + 1) % 3; }, valueButton);
    auto rxTotal = Button(&rxTotalLabel_,
        [this] { rxTotalUnit_ = (rxTotalUnit_ + 1) % 3; }, valueButton);
    auto txSpeed = Button(&txSpeedLabel_,
        [this] { txSpeedUnit_ = (txSpeedUnit_ + 1) % 3; }, valueButton);
    auto rxSpeed = Button(&rxSpeedLabel_,
        [this] { rxSpeedUnit_ = (rxSpeedUnit_ + 1) % 3; }, valueButton);

    auto controls = Container::Vertical({
        Container::Horizontal({refresh, wait, connect, disconnect}),
        clientList,
        Container::Horizontal({txTotal, txSpeed}),
        Container::Horizontal({rxTotal, rxSpeed}),
    });
    return Renderer(controls,
        [this, refresh, clientList, wait, connect, disconnect, txTotal,
         rxTotal, txSpeed, rxSpeed] {
        const auto& stats = engine_.GetStats();
        // One tick is the resolution of the counters that feed the indicator,
        // so anything newer than a tick and a half counts as live traffic.
        const auto now = std::chrono::steady_clock::now();
        const bool txActive = now - lastTxActivity_ < std::chrono::milliseconds(1500);
        const bool rxActive = now - lastRxActivity_ < std::chrono::milliseconds(1500);
        const int64_t rttMilliseconds = stats.rttMilliseconds.load();

        Elements peerRows{
            hbox({refresh->Render(), text("  "), wait->Render(), text("  "),
                  connect->Render(), text("  "), disconnect->Render(), filler(),
                  text(std::to_string(clients_.size()) + " online") | dim}),
            separator(),
        };
        if (clients_.empty()) {
            peerRows.push_back(vbox({
                filler(),
                hbox({filler(), text("No online peers. Press Refresh.") | dim,
                      filler()}),
                filler(),
            }) | flex);
        } else {
            peerRows.push_back(text("  " + clientHeader_) | bold);
            peerRows.push_back(clientList->Render()
                | vscroll_indicator | frame | flex);
            if (selectedClient_ >= 0
                && selectedClient_ < static_cast<int>(clientDetails_.size())) {
                const RendezvousPeerInfo& selected = clientDetails_[selectedClient_];
                peerRows.push_back(text("  " + selected.peerId + ": "
                    + FormatPeerCapabilities(selected.capabilities)) | dim);
            }
        }

        auto trafficRow = [](const std::string& name, bool active,
                             const std::string& packets, Element bytes,
                             Element speed) {
            return hbox({
                text(active ? " * " : " . ")
                    | color(active ? Color::GreenLight : Color::GrayDark),
                text(name) | bold | size(WIDTH, EQUAL, 4),
                text(packets) | size(WIDTH, EQUAL, 12),
                bytes | size(WIDTH, EQUAL, 16),
                speed | flex,
            });
        };
        Element traffic = vbox({
            hbox({
                // Skips the activity dot and the TX/RX name column so the
                // headers sit exactly above their values.
                text("") | size(WIDTH, EQUAL, 7),
                text("Packets") | bold | size(WIDTH, EQUAL, 12),
                text("Total") | bold | size(WIDTH, EQUAL, 16),
                text("Speed") | bold | flex,
            }),
            trafficRow("TX", txActive, std::to_string(stats.txPackets.load()),
                       txTotal->Render(), txSpeed->Render()),
            trafficRow("RX", rxActive, std::to_string(stats.rxPackets.load()),
                       rxTotal->Render(), rxSpeed->Render()),
            separator(),
            hbox({text(" Latency ") | bold,
                  text(rttMilliseconds < 0
                       ? "-- ms"
                       : std::to_string(rttMilliseconds) + " ms"),
                  filler(), text("TUN ring drops ") | bold,
                  text(std::to_string(stats.tunRingFullDrops.load())), text(" ")}),
            filler(),
        });

        // The peer list only needs as much room as a realistic room holds;
        // whatever is left goes to the charts, which read better when tall.
        return vbox({
            window(tui_theme::WindowTitle("Online peers"),
                   vbox(std::move(peerRows)))
                | size(HEIGHT, LESS_THAN, 13) | flex,
            hbox({
                window(tui_theme::WindowTitle("Traffic"), std::move(traffic))
                    | size(WIDTH, EQUAL, 46),
                window(tui_theme::WindowTitle("Last 60 seconds"),
                       RenderStatisticsCharts()) | flex,
            }) | size(HEIGHT, GREATER_THAN, 8) | flex,
        });
    });
}

// The Settings page edits numeric fields through text mirrors, so they are
// folded back into the config before validation and engine start.
bool TuiApp::StartConnection(const std::string& targetPeerId) {
    SyncConfigFromText();
    std::string error;
    if (!ValidateClientConfig(config_, &error)) { SetStatus(error); return false; }
    if (!targetPeerId.empty() && targetPeerId == config_.peerId) {
        SetStatus("Cannot connect to this client itself");
        return false;
    }
    return engine_.Start(ToEngineConfig(config_, targetPeerId));
}

void TuiApp::ConnectSelectedClient() {
    if (selectedClient_ < 0
        || selectedClient_ >= static_cast<int>(clientDetails_.size())) {
        SetStatus("Select an online client first");
        return;
    }
    const std::string target = clientDetails_[selectedClient_].peerId;
    engine_.Stop();
    StartConnection(target);
}

void TuiApp::Disconnect() {
    engine_.Stop();
    OnStateChanged(TunnelState::Disconnected, "Disconnected");
}

void TuiApp::RefreshClients() {
    SyncConfigFromText();
    std::string error;
    if (!ValidateClientConfig(config_, &error)) { SetStatus(error); return; }
    std::vector<RendezvousPeerInfo> clients;
    if (!ListRendezvousClients(config_.rendezvousAddress,
                               static_cast<uint16_t>(config_.rendezvousPort),
                               config_.roomId, config_.authToken, &clients, &error)) {
        SetStatus(error);
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
    clientDetails_ = std::move(clients);
    UpdateClientLabels();
    selectedClient_ = 0;
    SetStatus(clients_.empty() ? "No online clients" : "Client list refreshed");
}

// Renders every client as one aligned row so the radiobox shows the peer ID
// together with the details reported by the rendezvous server.
void TuiApp::UpdateClientLabels() {
    ClientColumns columns;
    std::vector<std::array<std::string, kClientColumnCount>> rows;
    rows.reserve(clientDetails_.size());
    for (const RendezvousPeerInfo& client : clientDetails_) {
        std::array<std::string, kClientColumnCount> row{
            client.peerId,
            PeerField(client.endpoint),
            SerializeTraversalModeSequence(client.capabilities),
            PeerField(client.tunIp),
            FormatIdleTime(client.idleSeconds),
        };
        for (size_t i = 0; i < kClientColumnCount; ++i) {
            columns.widths[i] = (std::max)(columns.widths[i],
                (std::min)(row[i].size(), kMaxClientColumnWidth));
        }
        rows.push_back(std::move(row));
    }

    clientHeader_ = JoinClientColumns(columns, kClientColumnTitles);
    clients_.clear();
    clients_.reserve(rows.size());
    for (const auto& row : rows) clients_.push_back(JoinClientColumns(columns, row));
}

void TuiApp::OnStateChanged(TunnelState state, const std::string& message) {
    state_.store(state);
    if (state == TunnelState::Disconnected || state == TunnelState::Error) {
        retryAfterMs_.store(SteadyMilliseconds()
            + static_cast<int64_t>(retryDelaySeconds_.load()) * 1000);
    }
    SetStatus(message.empty() ? "Disconnected" : message);
    screen_.PostEvent(ftxui::Event::Custom);
}

void TuiApp::SetStatus(const std::string& message) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_ = message;
}

void TuiApp::ProcessAutoWait() {
    if (exiting_.load() || !config_.autoWaitForPeer) return;
    const TunnelState state = state_.load();
    if (state != TunnelState::Disconnected && state != TunnelState::Error) return;
    const int64_t now = SteadyMilliseconds();
    if (now < retryAfterMs_.load()) return;
    if (!StartConnection("")) {
        retryAfterMs_.store(now
            + static_cast<int64_t>(retryDelaySeconds_.load()) * 1000);
    }
}

void TuiApp::UpdateStats() {
    const auto& stats = engine_.GetStats();
    const auto now = std::chrono::steady_clock::now();
    const uint64_t txPackets = stats.txPackets.load();
    const uint64_t rxPackets = stats.rxPackets.load();
    if (txPackets > observedTxPackets_) lastTxActivity_ = now;
    if (rxPackets > observedRxPackets_) lastRxActivity_ = now;
    observedTxPackets_ = txPackets;
    observedRxPackets_ = rxPackets;
#ifdef _WIN32
    if (windowsTray_) {
        const TunnelState state = state_.load();
        const bool unavailable = state == TunnelState::Disconnected
            || state == TunnelState::Waiting || state == TunnelState::Error;
        const bool connected = state == TunnelState::Connected;
        constexpr auto activityWindow = std::chrono::milliseconds(1500);
        windowsTray_->UpdateStatus(
            unavailable, connected && now - lastRxActivity_ < activityWindow,
            connected && now - lastTxActivity_ < activityWindow);
    }
#endif
    const uint64_t txBytes = stats.txBytes.load();
    const uint64_t rxBytes = stats.rxBytes.load();
    if (!speedInitialized_) {
        previousTxBytes_ = txBytes;
        previousRxBytes_ = rxBytes;
        lastSpeedSample_ = now;
        speedInitialized_ = true;
        return;
    }
    const double elapsed = std::chrono::duration<double>(now - lastSpeedSample_).count();
    if (elapsed < 1.0) return;
    txBytesPerSecond_ = static_cast<double>(
        txBytes >= previousTxBytes_ ? txBytes - previousTxBytes_ : 0) / elapsed;
    rxBytesPerSecond_ = static_cast<double>(
        rxBytes >= previousRxBytes_ ? rxBytes - previousRxBytes_ : 0) / elapsed;
    previousTxBytes_ = txBytes;
    previousRxBytes_ = rxBytes;
    lastSpeedSample_ = now;
}

// The traffic table carries the column headers, so the cells only hold the
// value and its currently selected unit.
void TuiApp::UpdateDisplayLabels() {
    const auto& stats = engine_.GetStats();
    txTotalLabel_ = ByteText(static_cast<double>(stats.txBytes.load()), txTotalUnit_, false);
    rxTotalLabel_ = ByteText(static_cast<double>(stats.rxBytes.load()), rxTotalUnit_, false);
    txSpeedLabel_ = ByteText(txBytesPerSecond_, txSpeedUnit_, true);
    rxSpeedLabel_ = ByteText(rxBytesPerSecond_, rxSpeedUnit_, true);
}
