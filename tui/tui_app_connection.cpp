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

#include "../log.h"
#include "../nat_protocol.h"
#include "../rendezvous_client.h"
#include "../util.h"

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

int ParseInt(const std::string& text, int fallback) {
    try {
        size_t consumed = 0;
        const int value = std::stoi(text, &consumed);
        return consumed == text.size() ? value : fallback;
    } catch (...) {
        return fallback;
    }
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
    auto refresh = Button("Refresh clients", [this] { RefreshClients(); });
    auto wait = Button("Wait for peer", [this] { StartConnection(""); });
    auto connect = Button("Connect selected", [this] { ConnectSelectedClient(); });
    auto disconnect = Button("Disconnect", [this] { Disconnect(); });
    auto txTotal = Button(&txTotalLabel_, [this] { txTotalUnit_ = (txTotalUnit_ + 1) % 3; });
    auto rxTotal = Button(&rxTotalLabel_, [this] { rxTotalUnit_ = (rxTotalUnit_ + 1) % 3; });
    auto txSpeed = Button(&txSpeedLabel_, [this] { txSpeedUnit_ = (txSpeedUnit_ + 1) % 3; });
    auto rxSpeed = Button(&rxSpeedLabel_, [this] { rxSpeedUnit_ = (rxSpeedUnit_ + 1) % 3; });
    ButtonOption compactStatusButton = ButtonOption::Simple();
    compactStatusButton.transform = [](const EntryState& entry) {
        Element output = text(entry.label);
        if (entry.focused) output = output | inverted;
        return output;
    };
    auto statusTx = Button(&statusTxLabel_,
        [this] { statusUnit_ = (statusUnit_ + 1) % 3; }, compactStatusButton);
    auto statusRx = Button(&statusRxLabel_,
        [this] { statusUnit_ = (statusUnit_ + 1) % 3; }, compactStatusButton);

    auto controls = Container::Vertical({
        refresh, clientList, wait, connect, disconnect, txTotal, rxTotal,
        txSpeed, rxSpeed, statusTx, statusRx,
    });
    return Renderer(controls,
        [this, refresh, clientList, wait, connect, disconnect, txTotal,
         rxTotal, txSpeed, rxSpeed, statusTx, statusRx] {
        const auto& stats = engine_.GetStats();
        std::string status;
        { std::lock_guard<std::mutex> lock(statusMutex_); status = status_; }
        const bool txActive = std::chrono::steady_clock::now() - lastTxActivity_
            < std::chrono::milliseconds(350);
        const bool rxActive = std::chrono::steady_clock::now() - lastRxActivity_
            < std::chrono::milliseconds(350);
        const int64_t rttMilliseconds = stats.rttMilliseconds.load();
        Elements clientRows{
            hbox({refresh->Render(), text("  Online clients:")}),
        };
        if (clients_.empty()) {
            clientRows.push_back(text("  No online clients") | dim);
        } else {
            clientRows.push_back(text("  " + clientHeader_) | bold | dim);
            clientRows.push_back(
                clientList->Render() | frame | size(HEIGHT, LESS_THAN, 7));
            if (selectedClient_ >= 0
                && selectedClient_ < static_cast<int>(clientDetails_.size())) {
                const RendezvousPeerInfo& selected = clientDetails_[selectedClient_];
                clientRows.push_back(text("  " + selected.peerId + ": "
                    + FormatPeerCapabilities(selected.capabilities)) | dim);
            }
        }
        return vbox({
            vbox(std::move(clientRows)),
            hbox({wait->Render(), connect->Render(), disconnect->Render()}),
            separator(),
            text("Packet statistics") | bold,
            hbox({
                vbox({
                    text("TX Packets: " + std::to_string(stats.txPackets.load())),
                    txTotal->Render(), txSpeed->Render(),
                }) | flex,
                vbox({
                    text("RX Packets: " + std::to_string(stats.rxPackets.load())),
                    rxTotal->Render(), rxSpeed->Render(),
                }) | flex,
            }),
            text(rttMilliseconds < 0
                ? "Latency: -- ms"
                : "Latency: " + std::to_string(rttMilliseconds) + " ms"),
            RenderStatisticsCharts(),
            separator(),
            hbox({
                text(txActive ? "* " : ". ")
                    | color(txActive ? Color::GreenLight : Color::GrayDark),
                text("TX "), statusTx->Render(), text("   "),
                text(rxActive ? "* " : ". ")
                    | color(rxActive ? Color::GreenLight : Color::GrayDark),
                text("RX "), statusRx->Render(),
            }),
            separator(),
            hbox({text("Status: ") | bold, text(status) | flex}),
        }) | vscroll_indicator | frame | border | flex;
    });
}

bool TuiApp::Validate(std::string* error) const {
    if (config_.rendezvousAddress.empty()) { *error = "Rendezvous server is required"; return false; }
    if (!IsSafeControlField(config_.roomId)) { *error = "Invalid Room ID"; return false; }
    if (!IsSafeControlField(config_.peerId)) { *error = "Invalid Peer ID"; return false; }
    if (!config_.authToken.empty() && !IsSafeControlField(config_.authToken)) {
        *error = "Invalid Auth Token"; return false;
    }
    in_addr address{};
    if (!ParseIpv4(config_.localTunIpv4, &address)) {
        *error = "Invalid Local TUN IPv4"; return false;
    }
    if (ParseInt(serverPortText_, 0) < 1 || ParseInt(serverPortText_, 0) > 65535) {
        *error = "Invalid rendezvous port"; return false;
    }
    const bool anyModeEnabled = std::any_of(
        config_.traversalModes.begin(), config_.traversalModes.end(),
        [](const auto& setting) { return setting.enabled; });
    if (!anyModeEnabled) {
        *error = "Enable at least one traversal mode"; return false;
    }
    const auto ipv6Mode = std::find_if(
        config_.traversalModes.begin(), config_.traversalModes.end(),
        [](const auto& setting) { return setting.mode == TraversalMode::Ipv6; });
    if (ipv6Mode != config_.traversalModes.end() && ipv6Mode->enabled
        && config_.ipv6ProbeHost.empty()) {
        *error = "IPv6 probe host is required"; return false;
    }
    return true;
}

Config TuiApp::BuildEngineConfig(const std::string& targetPeerId) const {
    Config output;
    output.rendezvous_addr = config_.rendezvousAddress;
    output.rendezvous_port = static_cast<uint16_t>(ParseInt(serverPortText_, 3478));
    output.room_id = config_.roomId;
    output.peer_id = config_.peerId;
    output.target_peer_id = targetPeerId;
    output.auth_token = config_.authToken;
    output.adapter_name = config_.adapterName;
    output.local_tun_ipv4 = config_.localTunIpv4;
    output.tun_prefix = static_cast<uint8_t>(std::clamp(ParseInt(tunPrefixText_, 24), 0, 32));
    output.tun_mtu = static_cast<uint16_t>(std::clamp(ParseInt(tunMtuText_, 1452), 576, 9000));
    output.auto_config_ipv4 = config_.autoConfigIpv4;
    output.keepalive_interval = static_cast<uint16_t>(
        std::clamp(ParseInt(keepaliveText_, 15), 1, 300));
    output.peer_timeout = static_cast<uint16_t>(
        std::clamp(ParseInt(peerTimeoutText_, 45), output.keepalive_interval + 1, 3600));
    output.dummy_traffic_enabled = config_.dummyTrafficEnabled;
    output.punch_timeout = static_cast<uint16_t>(
        std::clamp(ParseInt(punchTimeoutText_, 30), 1, 600));
    output.traversal_modes = config_.traversalModes;
    output.nat4_source_port_start = static_cast<uint16_t>(
        std::clamp(ParseInt(nat4SourcePortStartText_, 30000), 1, 65535));
    output.nat4_source_port_count = static_cast<uint16_t>(
        std::clamp(ParseInt(nat4SourcePortCountText_, 25), 1, 60));
    if (output.nat4_source_port_count > 0) {
        output.nat4_source_port_start = static_cast<uint16_t>((std::min)(
            static_cast<int>(output.nat4_source_port_start),
            65536 - static_cast<int>(output.nat4_source_port_count)));
    }
    output.nat4_peer_port_offset = static_cast<uint16_t>(
        std::clamp(ParseInt(nat4PeerPortOffsetText_, 20), 0, 256));
    output.nat4_round_timeout = static_cast<uint16_t>(
        std::clamp(ParseInt(nat4RoundTimeoutText_, 10), 1, 60));
    output.ipv6_accept_inbound = config_.ipv6AcceptInbound;
    output.ipv6_listen_port = static_cast<uint16_t>(
        std::clamp(ParseInt(ipv6ListenPortText_, 0), 0, 65535));
    output.ipv6_probe_host = config_.ipv6ProbeHost;
    output.ipv6_probe_port = static_cast<uint16_t>(
        std::clamp(ParseInt(ipv6ProbePortText_, 53), 1, 65535));
    output.ipv6_fallback_timeout = static_cast<uint16_t>(
        std::clamp(ParseInt(ipv6FallbackTimeoutText_, 15), 1, 120));
    TryParseLogLevel(logLevels_[std::clamp(config_.logLevel, 0, 3)], &output.log_level);
    return output;
}

bool TuiApp::StartConnection(const std::string& targetPeerId) {
    std::string error;
    if (!Validate(&error)) { SetStatus(error); return false; }
    if (!targetPeerId.empty() && targetPeerId == config_.peerId) {
        SetStatus("Cannot connect to this client itself");
        return false;
    }
    return engine_.Start(BuildEngineConfig(targetPeerId));
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
    std::string error;
    if (!Validate(&error)) { SetStatus(error); return; }
    std::vector<RendezvousPeerInfo> clients;
    if (!ListRendezvousClients(config_.rendezvousAddress,
                               static_cast<uint16_t>(ParseInt(serverPortText_, 3478)),
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

void TuiApp::UpdateDisplayLabels() {
    const auto& stats = engine_.GetStats();
    txTotalLabel_ = "TX Bytes: " + ByteText(static_cast<double>(stats.txBytes.load()), txTotalUnit_, false);
    rxTotalLabel_ = "RX Bytes: " + ByteText(static_cast<double>(stats.rxBytes.load()), rxTotalUnit_, false);
    txSpeedLabel_ = "TX Speed: " + ByteText(txBytesPerSecond_, txSpeedUnit_, true);
    rxSpeedLabel_ = "RX Speed: " + ByteText(rxBytesPerSecond_, rxSpeedUnit_, true);
    statusTxLabel_ = ByteText(static_cast<double>(stats.txBytes.load()), statusUnit_, false);
    statusRxLabel_ = ByteText(static_cast<double>(stats.rxBytes.load()), statusUnit_, false);
}
