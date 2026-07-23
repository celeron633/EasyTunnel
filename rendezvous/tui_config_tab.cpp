#include "tui_app.h"

#include <cstdint>
#include <limits>
#include <utility>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>

namespace {
bool ParseUInt16(const std::string& text, uint16_t* output) {
    try {
        size_t consumed = 0;
        const unsigned long value = std::stoul(text, &consumed);
        if (consumed != text.size() || value > 65535) return false;
        *output = static_cast<uint16_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}
}  // namespace

ftxui::Component RendezvousTuiApp::BuildConfigTab() {
    using namespace ftxui;
    auto bindAddress = Input(&config_.bindAddress, "0.0.0.0");
    auto port = Input(&portText_, "3478");
    InputOption password = InputOption::Default();
    password.password = true;
    auto token = Input(&config_.authToken, "optional shared secret", password);
    auto timeout = Input(&timeoutText_, "60");
    auto capacity = Input(&capacityText_, "32");
    auto relayEnabled = Checkbox("Enable IPv4 relay", &config_.ipv4RelayEnabled);
    auto relayPortStart = Input(&relayPortStartText_, "40000");
    auto relayPortEnd = Input(&relayPortEndText_, "40100");
    auto logLevel = Radiobox(&logLevels_, &logLevelIndex_);
    auto logFile = Input(&config_.logFile, "EasyTunnel_rendezvous.log");
    auto save = Button("Save", [this] { SaveConfig(false); });
    auto apply = Button("Save & restart", [this] { SaveConfig(true); });
    auto controls = Container::Vertical({bindAddress, port, token, timeout, capacity,
                                         relayEnabled, relayPortStart, relayPortEnd,
                                         logLevel, logFile,
                                         Container::Horizontal({save, apply})});
    return Renderer(controls, [this, bindAddress, port, token, timeout, capacity,
                               relayEnabled, relayPortStart, relayPortEnd,
                               logLevel, logFile, save, apply] {
        auto row = [](const std::string& label, Component component) {
            return hbox({text(label) | size(WIDTH, EQUAL, 28),
                         component->Render() | flex});
        };
        return vbox({
            text("Server configuration") | bold,
            separator(),
            row("Bind IPv4 address", bindAddress),
            row("UDP port", port),
            row("Authentication token", token),
            row("Client timeout (5..3600s)", timeout),
            row("Clients per room (2..32)", capacity),
            separator(),
            text("IPv4 relay") | bold,
            relayEnabled->Render(),
            row("Relay UDP port start", relayPortStart),
            row("Relay UDP port end", relayPortEnd),
            separator(),
            row("Log level", logLevel),
            row("Log file", logFile),
            separator(),
            hbox({save->Render(), text("  "), apply->Render()}),
            text("Save & restart applies server settings now; a new log file path is used on the next app launch.") | dim,
            separator(),
            text(configMessage_) | color(configMessageOk_ ? Color::Green : Color::Red),
        }) | border | flex;
    });
}

bool RendezvousTuiApp::ReadConfigEditor(RendezvousConfig* config,
                                        std::string* error) const {
    *config = config_;
    if (!ParseUInt16(portText_, &config->port) || config->port == 0) {
        *error = "port must be 1..65535";
        return false;
    }
    if (!ParseUInt16(timeoutText_, &config->clientTimeoutSeconds)) {
        *error = "client_timeout_seconds must be 5..3600";
        return false;
    }
    if (!ParseUInt16(capacityText_, &config->maxClientsPerRoom)) {
        *error = "max_clients_per_room must be 2..32";
        return false;
    }
    if (!ParseUInt16(relayPortStartText_, &config->ipv4RelayPortStart)
        || !ParseUInt16(relayPortEndText_, &config->ipv4RelayPortEnd)) {
        *error = "ipv4_relay_port_start/end must be valid UDP ports";
        return false;
    }
    config->logLevel = static_cast<LogLevel>(logLevelIndex_);
    return ValidateRendezvousConfig(*config, error);
}

void RendezvousTuiApp::SyncConfigEditor() {
    portText_ = std::to_string(config_.port);
    timeoutText_ = std::to_string(config_.clientTimeoutSeconds);
    capacityText_ = std::to_string(config_.maxClientsPerRoom);
    relayPortStartText_ = std::to_string(config_.ipv4RelayPortStart);
    relayPortEndText_ = std::to_string(config_.ipv4RelayPortEnd);
    logLevelIndex_ = static_cast<int>(config_.logLevel);
}

void RendezvousTuiApp::SaveConfig(bool restart) {
    RendezvousConfig updated;
    std::string error;
    if (!ReadConfigEditor(&updated, &error)
        || !SaveRendezvousConfig(configPath_, updated, &error)) {
        configMessage_ = error;
        configMessageOk_ = false;
        return;
    }
    config_ = std::move(updated);
    serverConfig_ = config_;
    SetLogLevel(config_.logLevel);
    configMessage_ = "Saved " + configPath_;
    configMessageOk_ = true;
    Log(LogLevel::Info, configMessage_);
    if (restart) RestartServer();
}
