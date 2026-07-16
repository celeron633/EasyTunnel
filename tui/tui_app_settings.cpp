#include "tui_app.h"

#include <algorithm>
#include <sstream>
#include <utility>

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

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

    auto controls = Container::Vertical({
        adapter, tunIp, tunPrefix, tunMtu, autoConfig, keepalive, peerTimeout,
        punchTimeout, nat4SourcePortStart, nat4SourcePortCount,
        nat4PeerPortOffset, nat4RoundTimeout, logLevel, dummyTraffic, autoWait,
    });
    return Renderer(controls,
        [this, adapter, tunIp, tunPrefix, tunMtu, autoConfig, keepalive,
         peerTimeout, punchTimeout, nat4SourcePortStart, nat4SourcePortCount,
         nat4PeerPortOffset, nat4RoundTimeout, logLevel, dummyTraffic, autoWait] {
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
            separator(),
            text("Log") | bold,
            row("Log Level", logLevel),
            separator(),
            text("Misc") | bold,
            dummyTraffic->Render(),
            autoWait->Render(),
            separator(),
            text(configMessage_) | color(configSaveOk_ ? Color::Green : Color::Red),
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
