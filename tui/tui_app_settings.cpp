#include "tui_app.h"

#include <algorithm>
#include <sstream>
#include <utility>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>

#include "tui_theme.h"

namespace {
int ParseInt(const std::string& text, int fallback) {
    try {
        size_t consumed = 0;
        const int value = std::stoi(text, &consumed);
        return consumed == text.size() ? value : fallback;
    } catch (...) {
        return fallback;
    }
}
}  // namespace

ftxui::Component TuiApp::BuildSettingsTab() {
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
    auto ipv6ListenPort = Input(&ipv6ListenPortText_, "0");
    auto ipv6ProbeHost = Input(&config_.ipv6ProbeHost, "2400:3200::1");
    auto ipv6ProbePort = Input(&ipv6ProbePortText_, "53");
    auto ipv6FallbackTimeout = Input(&ipv6FallbackTimeoutText_, "15");
    auto rendezvousRetryDelay = Input(&rendezvousRetryDelayText_, "5");
    auto autoConfig = Checkbox("Auto configure IPv4", &config_.autoConfigIpv4);
    auto dummyTraffic = Checkbox("1 KiB/s dummy traffic", &config_.dummyTrafficEnabled);
    auto ipv6Inbound = Checkbox("Accept inbound IPv6 UDP", &config_.ipv6AcceptInbound);
    auto autoWait = Checkbox("Auto wait for peer", &config_.autoWaitForPeer);
    // A horizontal toggle keeps the log level on a single row, which is what
    // lets the whole page fit a default-sized terminal window.
    auto logLevel = Toggle(&logLevels_, &config_.logLevel);

    const ButtonOption flatButton = tui_theme::FlatButton();
    Components modeCheckboxes;
    Components modeUpButtons;
    Components modeDownButtons;
    Components modeRows;
    for (size_t i = 0; i < config_.traversalModes.size(); ++i) {
        auto checkbox = Checkbox("", &config_.traversalModes[i].enabled);
        auto up = Button("^", [this, i] {
            if (i > 0) std::swap(config_.traversalModes[i], config_.traversalModes[i - 1]);
        }, flatButton);
        auto down = Button("v", [this, i] {
            if (i + 1 < config_.traversalModes.size()) {
                std::swap(config_.traversalModes[i], config_.traversalModes[i + 1]);
            }
        }, flatButton);
        modeCheckboxes.push_back(checkbox);
        modeUpButtons.push_back(up);
        modeDownButtons.push_back(down);
        modeRows.push_back(Container::Horizontal({checkbox, up, down}));
    }

    // The two columns are balanced so the whole page fits a default-sized
    // terminal window without scrolling: session settings on the left, the
    // data path on the right.
    auto leftColumn = Container::Vertical({
        serverAddress, serverPort, roomId, peerId, token, rendezvousRetryDelay,
        autoWait, keepalive, peerTimeout, punchTimeout,
        nat4SourcePortStart, nat4SourcePortCount,
        nat4PeerPortOffset, nat4RoundTimeout,
        logLevel, dummyTraffic,
    });
    Components rightControls = {adapter, tunIp, tunPrefix, tunMtu, autoConfig};
    rightControls.insert(rightControls.end(), modeRows.begin(), modeRows.end());
    rightControls.insert(rightControls.end(), {
        ipv6Inbound, ipv6ListenPort, ipv6ProbeHost,
        ipv6ProbePort, ipv6FallbackTimeout,
    });
    auto rightColumn = Container::Vertical(rightControls);
    auto controls = Container::Horizontal({leftColumn, rightColumn});
    return Renderer(controls,
        [this, adapter, tunIp, tunPrefix, tunMtu, autoConfig, keepalive,
         peerTimeout, punchTimeout, nat4SourcePortStart, nat4SourcePortCount,
         nat4PeerPortOffset, nat4RoundTimeout, logLevel, serverAddress,
         serverPort, roomId, peerId, token, rendezvousRetryDelay, dummyTraffic,
         autoWait, ipv6Inbound, ipv6ListenPort,
         ipv6ProbeHost, ipv6ProbePort, ipv6FallbackTimeout,
         modeCheckboxes, modeUpButtons, modeDownButtons] {
        const auto row = tui_theme::LabeledRow;
        Elements left = {
            tui_theme::SectionTitle("Rendezvous"),
            row("Server address", serverAddress),
            row("Server port", serverPort),
            row("Room ID", roomId),
            row("My peer ID", peerId),
            row("Auth token", token),
            row("Retry delay (s)", rendezvousRetryDelay),
            autoWait->Render(),
            separatorEmpty(),
            tui_theme::SectionTitle("NAT liveness"),
            row("Keepalive (s)", keepalive),
            row("Peer timeout (s)", peerTimeout),
            row("Punch timeout (s)", punchTimeout),
            row("NAT4 port start", nat4SourcePortStart),
            row("NAT4 port count", nat4SourcePortCount),
            row("NAT4 peer offset", nat4PeerPortOffset),
            row("NAT4 round timeout (s)", nat4RoundTimeout),
            separatorEmpty(),
            tui_theme::SectionTitle("Log and misc"),
            row("Log level", logLevel),
            dummyTraffic->Render(),
            filler(),
        };

        Elements right = {
            tui_theme::SectionTitle("TUN adapter"),
            row("Adapter name", adapter),
            row("Local TUN IPv4", tunIp),
            row("TUN prefix", tunPrefix),
            row("TUN MTU", tunMtu),
            autoConfig->Render(),
            separatorEmpty(),
            tui_theme::SectionTitle("Traversal strategy"),
            hbox({text("On") | size(WIDTH, EQUAL, 4),
                  text("#") | size(WIDTH, EQUAL, 3),
                  text("Mode") | flex,
                  text("Order")}),
        };
        for (size_t i = 0; i < config_.traversalModes.size(); ++i) {
            right.push_back(hbox({
                modeCheckboxes[i]->Render() | size(WIDTH, EQUAL, 4),
                text(std::to_string(i + 1)) | size(WIDTH, EQUAL, 3),
                text(TraversalModeDisplayName(config_.traversalModes[i].mode)) | flex,
                modeUpButtons[i]->Render(), text(" "), modeDownButtons[i]->Render(),
            }));
        }
        const Elements ipv6 = {
            separatorEmpty(),
            tui_theme::SectionTitle("IPv6 direct connection"),
            ipv6Inbound->Render(),
            row("Listen port (0=auto)", ipv6ListenPort),
            row("Probe host", ipv6ProbeHost),
            row("Probe TCP port", ipv6ProbePort),
            row("Probe timeout (s)", ipv6FallbackTimeout),
            filler(),
        };
        right.insert(right.end(), ipv6.begin(), ipv6.end());

        return vbox({
            hbox({
                vbox(std::move(left)) | vscroll_indicator | yframe | flex,
                separator(),
                vbox(std::move(right)) | vscroll_indicator | yframe | flex,
            }) | flex,
            separator(),
            text(configMessage_)
                | color(configSaveOk_ ? Color::Green : Color::Red),
        }) | border | flex;
    });
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
    ipv6ListenPortText_ = std::to_string(config_.ipv6ListenPort);
    ipv6ProbePortText_ = std::to_string(config_.ipv6ProbePort);
    ipv6FallbackTimeoutText_ = std::to_string(config_.ipv6FallbackTimeout);
    rendezvousRetryDelayText_ = std::to_string(config_.rendezvousRetryDelaySeconds);
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
        ParseInt(nat4SourcePortCountText_, config_.nat4SourcePortCount), 1, 60);
    if (config_.nat4SourcePortCount > 0) {
        config_.nat4SourcePortStart = (std::min)(
            config_.nat4SourcePortStart, 65536 - config_.nat4SourcePortCount);
    }
    config_.nat4PeerPortOffset = std::clamp(
        ParseInt(nat4PeerPortOffsetText_, config_.nat4PeerPortOffset), 0, 256);
    config_.nat4RoundTimeout = std::clamp(
        ParseInt(nat4RoundTimeoutText_, config_.nat4RoundTimeout), 1, 60);
    config_.ipv6ListenPort = std::clamp(
        ParseInt(ipv6ListenPortText_, config_.ipv6ListenPort), 0, 65535);
    config_.ipv6ProbePort = std::clamp(
        ParseInt(ipv6ProbePortText_, config_.ipv6ProbePort), 1, 65535);
    config_.ipv6FallbackTimeout = std::clamp(
        ParseInt(ipv6FallbackTimeoutText_, config_.ipv6FallbackTimeout), 1, 120);
    config_.rendezvousRetryDelaySeconds = std::clamp(
        ParseInt(rendezvousRetryDelayText_, config_.rendezvousRetryDelaySeconds), 1, 3600);
    retryDelaySeconds_.store(config_.rendezvousRetryDelaySeconds);
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
              << SerializeTraversalModes(config_.traversalModes) << '\n'
              << config_.ipv6AcceptInbound << '\n'
              << ipv6ListenPortText_ << '\n' << config_.ipv6ProbeHost << '\n'
              << ipv6ProbePortText_ << '\n' << ipv6FallbackTimeoutText_ << '\n'
              << config_.logLevel << '\n' << rendezvousRetryDelayText_ << '\n'
              << config_.dummyTrafficEnabled << '\n'
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
