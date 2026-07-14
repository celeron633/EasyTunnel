#include "tui_app.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "../log.h"
#include "../nat_protocol.h"
#include "../nat_traversal.h"
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
}  // namespace

ftxui::Component TuiApp::BuildConnectionTab() {
    using namespace ftxui;

    InputOption passwordOption = InputOption::Default();
    passwordOption.password = true;
    auto serverAddress = Input(&config_.rendezvousAddress, "server.example.com");
    auto serverPort = Input(&serverPortText_, "3478");
    auto roomId = Input(&config_.roomId, "room");
    auto peerId = Input(&config_.peerId, "peer-id");
    auto token = Input(&config_.authToken, "optional token", passwordOption);
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
        serverAddress, serverPort, roomId, peerId, token, refresh, clientList,
        wait, connect, disconnect, txTotal, rxTotal, txSpeed, rxSpeed,
        statusTx, statusRx,
    });
    return Renderer(controls,
        [this, serverAddress, serverPort, roomId, peerId, token, refresh,
         clientList, wait, connect, disconnect, txTotal, rxTotal, txSpeed,
         rxSpeed, statusTx, statusRx] {
        const auto& stats = engine_.GetStats();
        std::string status;
        { std::lock_guard<std::mutex> lock(statusMutex_); status = status_; }
        const bool txActive = std::chrono::steady_clock::now() - lastTxActivity_
            < std::chrono::milliseconds(350);
        const bool rxActive = std::chrono::steady_clock::now() - lastRxActivity_
            < std::chrono::milliseconds(350);
        auto row = [](const std::string& label, Component component) {
            return hbox({text(label) | size(WIDTH, EQUAL, 26), component->Render() | flex});
        };
        return vbox({
            text("Rendezvous") | bold,
            separator(),
            row("Rendezvous Server Addr", serverAddress),
            row("Rendezvous Server Port", serverPort),
            row("Room ID", roomId),
            row("My Peer ID", peerId),
            row("Auth Token", token),
            separator(),
            hbox({refresh->Render(), text("  Online clients:")}),
            clientList->Render() | frame | size(HEIGHT, LESS_THAN, 7),
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
        }) | border | flex;
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
    output.nat4_source_port_start = static_cast<uint16_t>(
        std::clamp(ParseInt(nat4SourcePortStartText_, 30000), 1, 65535));
    output.nat4_source_port_count = static_cast<uint16_t>(
        std::clamp(ParseInt(nat4SourcePortCountText_, 25), 0, 60));
    if (output.nat4_source_port_count > 0) {
        output.nat4_source_port_start = static_cast<uint16_t>((std::min)(
            static_cast<int>(output.nat4_source_port_start),
            65536 - static_cast<int>(output.nat4_source_port_count)));
    }
    output.nat4_peer_port_offset = static_cast<uint16_t>(
        std::clamp(ParseInt(nat4PeerPortOffsetText_, 20), 0, 256));
    output.nat4_round_timeout = static_cast<uint16_t>(
        std::clamp(ParseInt(nat4RoundTimeoutText_, 10), 1, 60));
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
    if (clients_.empty() || selectedClient_ < 0
        || selectedClient_ >= static_cast<int>(clients_.size())) {
        SetStatus("Select an online client first");
        return;
    }
    const std::string target = clients_[selectedClient_];
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
    std::vector<std::string> clients;
    if (!ListRendezvousClients(config_.rendezvousAddress,
                               static_cast<uint16_t>(ParseInt(serverPortText_, 3478)),
                               config_.roomId, config_.authToken, &clients, &error)) {
        SetStatus(error);
        return;
    }
    clients.erase(std::remove(clients.begin(), clients.end(), config_.peerId), clients.end());
    std::sort(clients.begin(), clients.end());
    clients_ = std::move(clients);
    selectedClient_ = 0;
    SetStatus(clients_.empty() ? "No online clients" : "Client list refreshed");
}

void TuiApp::OnStateChanged(TunnelState state, const std::string& message) {
    state_.store(state);
    if (state == TunnelState::Disconnected || state == TunnelState::Error) {
        retryAfterMs_.store(state == TunnelState::Error ? SteadyMilliseconds() + 1000 : 0);
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
    if (!StartConnection("")) retryAfterMs_.store(now + 1000);
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
