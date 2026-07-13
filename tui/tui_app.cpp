#include "tui_app.h"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <sstream>

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

TuiApp::TuiApp() : screen_(ftxui::ScreenInteractive::Fullscreen()) {}

TuiApp::~TuiApp() {
    exiting_.store(true);
    StopTicker();
    SetLogCallback({});
    engine_.Stop();
}

bool TuiApp::Init() {
    try {
        configPath_ = std::filesystem::absolute("EasyTunnel_tui.json").string();
    } catch (...) {
        configPath_ = "EasyTunnel_tui.json";
    }
    bool existed = false;
    std::string error;
    if (!LoadTuiConfig(configPath_, &config_, &existed, &error)) {
        configMessage_ = error;
        configSaveOk_ = false;
    }
    SyncTextFromConfig();
    if (!existed) {
        if (SaveTuiConfig(configPath_, config_, &error)) {
            configMessage_ = "Created " + configPath_;
            configSaveOk_ = true;
        } else {
            configMessage_ = error;
            configSaveOk_ = false;
        }
    } else if (configSaveOk_) {
        configMessage_ = "Loaded " + configPath_;
    }
    savedSignature_ = ConfigSignature();
    SetLogCallback([this](LogLevel level, const std::string& message) {
        OnLog(level, message);
    });
    engine_.SetStateCallback([this](TunnelState state, const std::string& message) {
        OnStateChanged(state, message);
    });
    UpdateDisplayLabels();
    return true;
}

int TuiApp::Run() {
    using namespace ftxui;

    InputOption passwordOption = InputOption::Default();
    passwordOption.password = true;
    auto serverAddress = Input(&config_.rendezvousAddress, "server.example.com");
    auto serverPort = Input(&serverPortText_, "3478");
    auto roomId = Input(&config_.roomId, "room");
    auto peerId = Input(&config_.peerId, "peer-id");
    auto token = Input(&config_.authToken, "optional token", passwordOption);
    auto adapter = Input(&config_.adapterName, "EasyTunnel");
    auto tunIp = Input(&config_.localTunIpv4, "10.66.0.1");
    auto tunPrefix = Input(&tunPrefixText_, "24");
    auto tunMtu = Input(&tunMtuText_, "1452");
    auto keepalive = Input(&keepaliveText_, "15");
    auto peerTimeout = Input(&peerTimeoutText_, "45");
    auto punchTimeout = Input(&punchTimeoutText_, "30");
    auto nat4SourcePortStart = Input(&nat4SourcePortStartText_, "30000");
    auto nat4SourcePortCount = Input(&nat4SourcePortCountText_, "25");
    auto nat4PeerPortOffset = Input(&nat4PeerPortOffsetText_, "20");
    auto nat4RoundTimeout = Input(&nat4RoundTimeoutText_, "10");
    auto autoConfig = Checkbox("Auto configure IPv4", &config_.autoConfigIpv4);
    auto dummyTraffic = Checkbox("1 KiB/s dummy traffic", &config_.dummyTrafficEnabled);
    auto autoWait = Checkbox("Auto wait for peer", &config_.autoWaitForPeer);
    auto logLevel = Radiobox(&logLevels_, &config_.logLevel);
    auto clientList = Radiobox(&clients_, &selectedClient_);

    auto refresh = Button("Refresh clients", [this] { RefreshClients(); });
    auto wait = Button("Wait for peer", [this] { StartConnection(""); });
    auto connect = Button("Connect selected", [this] { ConnectSelectedClient(); });
    auto disconnect = Button("Disconnect", [this] { Disconnect(); });
    auto clearLog = Button("Clear log", [this] {
        std::lock_guard<std::mutex> lock(logMutex_);
        logLines_.clear();
    });
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
    auto quit = Button("Quit", screen_.ExitLoopClosure());

    auto connectionControls = Container::Vertical({
        serverAddress, serverPort, roomId, peerId, token, refresh, clientList,
        wait, connect, disconnect, txTotal, rxTotal, txSpeed, rxSpeed,
    });
    auto connection = Renderer(connectionControls, [&, this] {
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
                // ASCII-only activity indicator. Avoid East-Asian *ambiguous*
                // width glyphs (e.g. "●"/"·"): a CJK Windows console draws them
                // 2 cells wide while FTXUI measures 1, which desyncs the line and
                // makes the whole panel flicker on every redraw. '*'/'.' are
                // fixed width-1 in both, so the layout stays stable.
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

    auto settingsControls = Container::Vertical({
        adapter, tunIp, tunPrefix, tunMtu, autoConfig, keepalive, peerTimeout,
        punchTimeout, nat4SourcePortStart, nat4SourcePortCount,
        nat4PeerPortOffset, nat4RoundTimeout, logLevel, dummyTraffic, autoWait,
    });
    auto settings = Renderer(settingsControls, [&, this] {
        auto row = [](const std::string& label, Component component) {
            return hbox({text(label) | size(WIDTH, EQUAL, 26), component->Render() | flex});
        };
        return vbox({
            text("TUN adapter") | bold,
            separator(),
            row("Adapter Name", adapter),
            row("Local TUN IPv4", tunIp),
            row("TUN Prefix", tunPrefix),
            row("TUN MTU", tunMtu),
            autoConfig->Render(),
            separator(),
            text("NAT liveness") | bold,
            row("Keepalive Seconds", keepalive),
            row("Peer Timeout Seconds", peerTimeout),
            row("Punch Timeout Seconds", punchTimeout),
            row("NAT4 Source Port Start", nat4SourcePortStart),
            row("NAT4 Source Port Count", nat4SourcePortCount),
            row("NAT4 Peer Port Offset", nat4PeerPortOffset),
            row("NAT4 Round Timeout", nat4RoundTimeout),
            row("Log Level", logLevel),
            separator(),
            text("Misc") | bold,
            dummyTraffic->Render(),
            autoWait->Render(),
            separator(),
            text(configMessage_) | color(configSaveOk_ ? Color::Green : Color::Red),
        }) | border | flex;
    });

    auto logControls = Container::Vertical({clearLog});
    auto logs = Renderer(logControls, [&, this] {
        Elements lines;
        {
            std::lock_guard<std::mutex> lock(logMutex_);
            const size_t begin = logLines_.size() > 24 ? logLines_.size() - 24 : 0;
            for (size_t i = begin; i < logLines_.size(); ++i) lines.push_back(text(logLines_[i]));
        }
        if (lines.empty()) lines.push_back(text("No log messages"));
        return vbox({
            hbox({text("Log") | bold, filler(), clearLog->Render()}),
            separator(),
            vbox(std::move(lines)) | frame | flex,
        }) | border | flex;
    });

    auto tabs = Toggle(&tabs_, &selectedTab_);
    auto tabContent = Container::Tab({connection, settings, logs}, &selectedTab_);
    auto rootContainer = Container::Vertical({tabs, tabContent, quit});
    auto root = Renderer(rootContainer, [&, this] {
        return vbox({
            hbox({text(" EasyTunnel TUI ") | bold, filler(), tabs->Render(), filler(), quit->Render()}),
            separator(),
            tabContent->Render() | flex,
        });
    });
    root |= CatchEvent([this](Event event) {
        if (event == Event::Custom) {
            UpdateStats();
            ProcessAutoWait();
            SaveIfChanged();
            UpdateDisplayLabels();
            return true;
        }
        return false;
    });

    tickerRunning_.store(true);
    tickerThread_ = std::thread([this] {
        while (tickerRunning_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (tickerRunning_.load()) screen_.PostEvent(Event::Custom);
        }
    });
    screen_.Loop(root);
    exiting_.store(true);
    StopTicker();
    engine_.Stop();
    SaveIfChanged();
    return 0;
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
    const bool started = engine_.Start(BuildEngineConfig(targetPeerId));
    return started;
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

void TuiApp::OnLog(LogLevel /*level*/, const std::string& message) {
    std::lock_guard<std::mutex> lock(logMutex_);
    logLines_.push_back(message);
    if (logLines_.size() > 2000) logLines_.erase(logLines_.begin(), logLines_.begin() + 500);
    screen_.PostEvent(ftxui::Event::Custom);
}

void TuiApp::SetStatus(const std::string& message) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_ = message;
}

void TuiApp::ProcessAutoWait() {
    if (exiting_.load() || !config_.autoWaitForPeer) {
        return;
    }
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

void TuiApp::SyncTextFromConfig() {
    serverPortText_ = std::to_string(config_.rendezvousPort);
    tunPrefixText_ = std::to_string(config_.tunPrefix);
    tunMtuText_ = std::to_string(config_.tunMtu);
    keepaliveText_ = std::to_string(config_.keepaliveInterval);
    peerTimeoutText_ = std::to_string(config_.peerTimeout);
    punchTimeoutText_ = std::to_string(config_.punchTimeout);
    nat4SourcePortStartText_ = std::to_string(config_.nat4SourcePortStart);
    nat4SourcePortCountText_ = std::to_string(config_.nat4SourcePortCount);
    nat4PeerPortOffsetText_ = std::to_string(config_.nat4PeerPortOffset);
    nat4RoundTimeoutText_ = std::to_string(config_.nat4RoundTimeout);
}

void TuiApp::SyncConfigFromText() {
    config_.rendezvousPort = std::clamp(ParseInt(serverPortText_, config_.rendezvousPort), 1, 65535);
    config_.tunPrefix = std::clamp(ParseInt(tunPrefixText_, config_.tunPrefix), 0, 32);
    config_.tunMtu = std::clamp(ParseInt(tunMtuText_, config_.tunMtu), 576, 9000);
    config_.keepaliveInterval = std::clamp(
        ParseInt(keepaliveText_, config_.keepaliveInterval), 1, 300);
    config_.peerTimeout = std::clamp(ParseInt(peerTimeoutText_, config_.peerTimeout),
                                     config_.keepaliveInterval + 1, 3600);
    config_.punchTimeout = std::clamp(ParseInt(punchTimeoutText_, config_.punchTimeout), 1, 600);
    config_.nat4SourcePortStart = std::clamp(
        ParseInt(nat4SourcePortStartText_, config_.nat4SourcePortStart), 1, 65535);
    config_.nat4SourcePortCount = std::clamp(
        ParseInt(nat4SourcePortCountText_, config_.nat4SourcePortCount), 0, 60);
    if (config_.nat4SourcePortCount > 0) {
        config_.nat4SourcePortStart = (std::min)(
            config_.nat4SourcePortStart, 65536 - config_.nat4SourcePortCount);
    }
    config_.nat4PeerPortOffset = std::clamp(
        ParseInt(nat4PeerPortOffsetText_, config_.nat4PeerPortOffset), 0, 256);
    config_.nat4RoundTimeout = std::clamp(
        ParseInt(nat4RoundTimeoutText_, config_.nat4RoundTimeout), 1, 60);
}

std::string TuiApp::ConfigSignature() const {
    std::ostringstream signature;
    signature << config_.rendezvousAddress << '\n' << serverPortText_ << '\n'
              << config_.roomId << '\n' << config_.peerId << '\n' << config_.authToken << '\n'
              << config_.adapterName << '\n' << config_.localTunIpv4 << '\n'
              << tunPrefixText_ << '\n' << tunMtuText_ << '\n' << config_.autoConfigIpv4 << '\n'
              << keepaliveText_ << '\n' << peerTimeoutText_ << '\n' << punchTimeoutText_ << '\n'
              << nat4SourcePortStartText_ << '\n' << nat4SourcePortCountText_ << '\n'
              << nat4PeerPortOffsetText_ << '\n' << nat4RoundTimeoutText_ << '\n'
              << config_.logLevel << '\n' << config_.dummyTrafficEnabled << '\n'
              << config_.autoWaitForPeer;
    return signature.str();
}

void TuiApp::SaveIfChanged() {
    const std::string signature = ConfigSignature();
    if (signature == savedSignature_) return;
    SyncConfigFromText();
    std::string error;
    if (SaveTuiConfig(configPath_, config_, &error)) {
        configMessage_ = "Configuration saved: " + configPath_;
        configSaveOk_ = true;
        savedSignature_ = signature;
    } else {
        configMessage_ = error;
        configSaveOk_ = false;
    }
}

void TuiApp::StopTicker() {
    tickerRunning_.store(false);
    if (tickerThread_.joinable()) tickerThread_.join();
}
