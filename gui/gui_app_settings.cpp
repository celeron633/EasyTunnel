#include "gui_app.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>

#include "imgui.h"

#include "../log.h"

namespace {
constexpr float kFormLabelWidth = 155.0f;
constexpr auto kConfigSaveMessageDuration = std::chrono::seconds(3);

bool BeginForm(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, kFormLabelWidth);
    ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void EndForm() { ImGui::EndTable(); }

void FormField(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

void FormMessage(const ImVec4& color, const char* text) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    ImGui::TextColored(color, "%s", text);
}

std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

size_t JsonValueStart(const std::string& json, const std::string& key) {
    const size_t keyPos = json.find("\"" + key + "\"");
    if (keyPos == std::string::npos) return std::string::npos;
    const size_t colon = json.find(':', keyPos + key.size() + 2);
    if (colon == std::string::npos) return std::string::npos;
    return json.find_first_not_of(" \t\r\n", colon + 1);
}

bool JsonString(const std::string& json, const std::string& key, std::string* value) {
    size_t pos = JsonValueStart(json, key);
    if (pos == std::string::npos || json[pos] != '"') return false;
    ++pos;
    std::string out;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (escaped) {
            switch (c) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                default: return false;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            *value = std::move(out);
            return true;
        } else {
            out += c;
        }
    }
    return false;
}

bool JsonInt(const std::string& json, const std::string& key, int* value) {
    const size_t pos = JsonValueStart(json, key);
    if (pos == std::string::npos) return false;
    try {
        size_t consumed = 0;
        const int parsed = std::stoi(json.substr(pos), &consumed);
        if (consumed == 0) return false;
        *value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool JsonBool(const std::string& json, const std::string& key, bool* value) {
    const size_t pos = JsonValueStart(json, key);
    if (pos == std::string::npos) return false;
    if (json.compare(pos, 4, "true") == 0) { *value = true; return true; }
    if (json.compare(pos, 5, "false") == 0) { *value = false; return true; }
    return false;
}

template <size_t N>
void CopyToBuffer(const std::string& value, char (&buffer)[N]) {
    std::snprintf(buffer, N, "%s", value.c_str());
}
}  // namespace

void GuiApp::RenderSettingsTab() {
    bool configChanged = false;
    ImGui::Spacing();
    ImGui::SeparatorText("Rendezvous");
    if (BeginForm("##RendezvousSettings")) {
        const TunnelState state = currentState_.load();
        const bool active = state == TunnelState::Connecting || state == TunnelState::Connected;
        if (active) ImGui::BeginDisabled();
        FormField("Rendezvous Server Addr");
        configChanged |= ImGui::InputText(
            "##ServerAddress", serverAddress_, sizeof(serverAddress_));
        FormField("Rendezvous Server Port");
        configChanged |= ImGui::InputInt("##ServerPort", &serverPort_);
        serverPort_ = std::clamp(serverPort_, 1, 65535);
        FormField("Room ID");
        configChanged |= ImGui::InputText("##RoomId", roomId_, sizeof(roomId_));
        FormField("My Peer ID");
        configChanged |= ImGui::InputText("##PeerId", peerId_, sizeof(peerId_));
        FormField("Auth Token");
        configChanged |= ImGui::InputText("##AuthToken", authToken_, sizeof(authToken_),
                                          ImGuiInputTextFlags_Password);
        if (active) ImGui::EndDisabled();
        FormField("Retry Delay Seconds");
        const bool retryDelayChanged = ImGui::InputInt(
            "##RendezvousRetryDelay", &rendezvousRetryDelaySeconds_);
        rendezvousRetryDelaySeconds_ = std::clamp(rendezvousRetryDelaySeconds_, 1, 3600);
        if (retryDelayChanged) {
            autoWaitRetryDelaySecondsRuntime_.store(rendezvousRetryDelaySeconds_);
        }
        configChanged |= retryDelayChanged;
        FormField("Auto wait for peer");
        const bool autoWaitChanged = ImGui::Checkbox("##AutoWaitForPeer", &autoWaitForPeer_);
        configChanged |= autoWaitChanged;
        if (autoWaitChanged) {
            autoWaitEnabledRuntime_.store(autoWaitForPeer_);
            if (autoWaitForPeer_) {
                autoWaitRetryAfterMs_.store(0);
                const TunnelState currentState = currentState_.load();
                if (currentState == TunnelState::Disconnected
                    || currentState == TunnelState::Error) {
                    autoWaitPending_.store(true);
                }
            } else {
                autoWaitPending_.store(false);
            }
        }
        EndForm();
    }
    ImGui::Spacing();
    ImGui::SeparatorText("TUN adapter");
    if (BeginForm("##TunSettings")) {
        FormField("Adapter Name");
        configChanged |= ImGui::InputText("##AdapterName", adapterName_, sizeof(adapterName_));
        FormField("Local TUN IPv4");
        configChanged |= ImGui::InputText("##TunIpv4", localTunIpv4_, sizeof(localTunIpv4_));
        FormField("TUN Prefix");
        configChanged |= ImGui::SliderInt("##TunPrefix", &tunPrefix_, 0, 32);
        FormField("TUN MTU");
        configChanged |= ImGui::InputInt("##TunMtu", &tunMtu_);
        tunMtu_ = std::clamp(tunMtu_, 576, 9000);
        if (tunMtu_ > 1472) {
            FormMessage(ImVec4(1.0f, 0.8f, 0.0f, 1.0f),
                        "MTU > 1472 may cause outer IPv4 fragmentation");
        }
        FormField("Auto Configure IPv4");
        configChanged |= ImGui::Checkbox("##AutoConfig", &autoConfigIpv4_);
        EndForm();
    }
    ImGui::Spacing();
    ImGui::SeparatorText("NAT liveness");
    if (BeginForm("##NatSettings")) {
        FormField("Keepalive Seconds");
        configChanged |= ImGui::InputInt("##Keepalive", &keepaliveInterval_);
        keepaliveInterval_ = std::clamp(keepaliveInterval_, 1, 300);
        FormField("Peer Timeout Seconds");
        configChanged |= ImGui::InputInt("##PeerTimeout", &peerTimeout_);
        peerTimeout_ = std::clamp(peerTimeout_, keepaliveInterval_ + 1, 3600);
        FormField("Punch Timeout Seconds");
        configChanged |= ImGui::InputInt("##PunchTimeout", &punchTimeout_);
        punchTimeout_ = std::clamp(punchTimeout_, 1, 600);
        FormField("NAT4 Source Port Start");
        configChanged |= ImGui::InputInt("##Nat4SourcePortStart", &nat4SourcePortStart_);
        nat4SourcePortStart_ = std::clamp(nat4SourcePortStart_, 1, 65535);
        FormField("NAT4 Source Port Count");
        configChanged |= ImGui::InputInt("##Nat4SourcePortCount", &nat4SourcePortCount_);
        nat4SourcePortCount_ = std::clamp(nat4SourcePortCount_, 0, 60);
        if (nat4SourcePortCount_ > 0) {
            nat4SourcePortStart_ = (std::min)(
                nat4SourcePortStart_, 65536 - nat4SourcePortCount_);
        }
        FormField("NAT4 Peer Port Offset");
        configChanged |= ImGui::InputInt("##Nat4PeerPortOffset", &nat4PeerPortOffset_);
        nat4PeerPortOffset_ = std::clamp(nat4PeerPortOffset_, 0, 256);
        FormField("NAT4 Round Timeout Seconds");
        configChanged |= ImGui::InputInt("##Nat4RoundTimeout", &nat4RoundTimeout_);
        nat4RoundTimeout_ = std::clamp(nat4RoundTimeout_, 1, 60);
        EndForm();
    }
    ImGui::Spacing();
    ImGui::SeparatorText("Log");
    if (BeginForm("##LogSettings")) {
        FormField("Log Level");
        configChanged |= ImGui::Combo("##LogLevel", &logLevelIdx_, kLogLevels, kLogLevelCount);
        EndForm();
    }
    ImGui::Spacing();
    ImGui::SeparatorText("IPv6 Fallback");
    if (BeginForm("##Ipv6FallbackSettings")) {
        FormField("Enable Fallback");
        configChanged |= ImGui::Checkbox("##Ipv6Fallback", &ipv6FallbackEnabled_);
        FormField("Accept Inbound UDP");
        configChanged |= ImGui::Checkbox("##Ipv6Inbound", &ipv6AcceptInbound_);
        FormField("Listen Port (0=auto)");
        configChanged |= ImGui::InputInt("##Ipv6ListenPort", &ipv6ListenPort_);
        ipv6ListenPort_ = std::clamp(ipv6ListenPort_, 0, 65535);
        FormField("Probe Host");
        configChanged |= ImGui::InputText(
            "##Ipv6ProbeHost", ipv6ProbeHost_, sizeof(ipv6ProbeHost_));
        FormField("Probe TCP Port");
        configChanged |= ImGui::InputInt("##Ipv6ProbePort", &ipv6ProbePort_);
        ipv6ProbePort_ = std::clamp(ipv6ProbePort_, 1, 65535);
        FormField("Timeout Seconds");
        configChanged |= ImGui::InputInt("##Ipv6FallbackTimeout", &ipv6FallbackTimeout_);
        ipv6FallbackTimeout_ = std::clamp(ipv6FallbackTimeout_, 1, 120);
        EndForm();
    }
    ImGui::Spacing();
    ImGui::SeparatorText("IPv4 Relay Fallback");
    if (BeginForm("##Ipv4RelayFallbackSettings")) {
        FormField("Enable Fallback");
        configChanged |= ImGui::Checkbox(
            "##Ipv4RelayFallback", &ipv4RelayFallbackEnabled_);
        EndForm();
    }
    ImGui::Spacing();
    ImGui::SeparatorText("Misc");
    if (BeginForm("##MiscSettings")) {
        FormField("1 KiB/s dummy traffic");
        configChanged |= ImGui::Checkbox("##DummyTraffic", &dummyTrafficEnabled_);
        EndForm();
    }
    if (configChanged) SaveGuiConfig();
    ImGui::Spacing();
    RenderConfigSaveStatus();
}

bool GuiApp::LoadGuiConfig() {
    std::ifstream input(configFilePath_, std::ios::binary);
    if (!input.is_open()) {
        ShowConfigSaveMessage("Configuration will be saved to " + configFilePath_, true);
        return true;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    const std::string json = contents.str();
    if (json.find('{') == std::string::npos || json.find('}') == std::string::npos) {
        const std::string message = "Invalid configuration JSON: " + configFilePath_;
        ShowConfigSaveMessage(message, false);
        Log(LogLevel::Error, message);
        return false;
    }
    std::string text;
    if (JsonString(json, "rendezvous_address", &text)) CopyToBuffer(text, serverAddress_);
    JsonInt(json, "rendezvous_port", &serverPort_);
    if (JsonString(json, "room_id", &text)) CopyToBuffer(text, roomId_);
    if (JsonString(json, "peer_id", &text)) CopyToBuffer(text, peerId_);
    if (JsonString(json, "auth_token", &text)) CopyToBuffer(text, authToken_);
    if (JsonString(json, "adapter_name", &text)) CopyToBuffer(text, adapterName_);
    if (JsonString(json, "local_tun_ipv4", &text)) CopyToBuffer(text, localTunIpv4_);
    JsonInt(json, "tun_prefix", &tunPrefix_);
    JsonInt(json, "tun_mtu", &tunMtu_);
    JsonBool(json, "auto_config_ipv4", &autoConfigIpv4_);
    JsonInt(json, "keepalive_interval", &keepaliveInterval_);
    JsonInt(json, "peer_timeout", &peerTimeout_);
    JsonBool(json, "dummy_traffic_enabled", &dummyTrafficEnabled_);
    JsonInt(json, "punch_timeout", &punchTimeout_);
    JsonInt(json, "nat4_source_port_start", &nat4SourcePortStart_);
    JsonInt(json, "nat4_source_port_count", &nat4SourcePortCount_);
    JsonInt(json, "nat4_peer_port_offset", &nat4PeerPortOffset_);
    JsonInt(json, "nat4_round_timeout", &nat4RoundTimeout_);
    JsonBool(json, "ipv6_fallback_enabled", &ipv6FallbackEnabled_);
    JsonBool(json, "ipv6_accept_inbound", &ipv6AcceptInbound_);
    JsonInt(json, "ipv6_listen_port", &ipv6ListenPort_);
    if (JsonString(json, "ipv6_probe_host", &text)) CopyToBuffer(text, ipv6ProbeHost_);
    JsonInt(json, "ipv6_probe_port", &ipv6ProbePort_);
    JsonInt(json, "ipv6_fallback_timeout", &ipv6FallbackTimeout_);
    JsonBool(json, "ipv4_relay_fallback_enabled",
             &ipv4RelayFallbackEnabled_);
    JsonInt(json, "log_level", &logLevelIdx_);
    JsonInt(json, "rendezvous_retry_delay_seconds", &rendezvousRetryDelaySeconds_);
    JsonBool(json, "auto_wait_for_peer", &autoWaitForPeer_);

    serverPort_ = std::clamp(serverPort_, 1, 65535);
    tunPrefix_ = std::clamp(tunPrefix_, 0, 32);
    tunMtu_ = std::clamp(tunMtu_, 576, 9000);
    keepaliveInterval_ = std::clamp(keepaliveInterval_, 1, 300);
    peerTimeout_ = std::clamp(peerTimeout_, keepaliveInterval_ + 1, 3600);
    punchTimeout_ = std::clamp(punchTimeout_, 1, 600);
    nat4SourcePortStart_ = std::clamp(nat4SourcePortStart_, 1, 65535);
    nat4SourcePortCount_ = std::clamp(nat4SourcePortCount_, 0, 60);
    if (nat4SourcePortCount_ > 0) {
        nat4SourcePortStart_ = (std::min)(
            nat4SourcePortStart_, 65536 - nat4SourcePortCount_);
    }
    nat4PeerPortOffset_ = std::clamp(nat4PeerPortOffset_, 0, 256);
    nat4RoundTimeout_ = std::clamp(nat4RoundTimeout_, 1, 60);
    ipv6ListenPort_ = std::clamp(ipv6ListenPort_, 0, 65535);
    ipv6ProbePort_ = std::clamp(ipv6ProbePort_, 1, 65535);
    ipv6FallbackTimeout_ = std::clamp(ipv6FallbackTimeout_, 1, 120);
    logLevelIdx_ = std::clamp(logLevelIdx_, 0, kLogLevelCount - 1);
    rendezvousRetryDelaySeconds_ = std::clamp(rendezvousRetryDelaySeconds_, 1, 3600);
    const std::string message = "Configuration loaded from " + configFilePath_;
    ShowConfigSaveMessage(message, true);
    Log(LogLevel::Info, message);
    return true;
}

bool GuiApp::SaveGuiConfig() {
    std::ofstream output(configFilePath_, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        const std::string message = "Failed to save configuration: " + configFilePath_;
        ShowConfigSaveMessage(message, false);
        Log(LogLevel::Error, message);
        return false;
    }
    output
        << "{\n"
        << "  \"rendezvous_address\": \"" << JsonEscape(serverAddress_) << "\",\n"
        << "  \"rendezvous_port\": " << serverPort_ << ",\n"
        << "  \"room_id\": \"" << JsonEscape(roomId_) << "\",\n"
        << "  \"peer_id\": \"" << JsonEscape(peerId_) << "\",\n"
        << "  \"auth_token\": \"" << JsonEscape(authToken_) << "\",\n"
        << "  \"adapter_name\": \"" << JsonEscape(adapterName_) << "\",\n"
        << "  \"local_tun_ipv4\": \"" << JsonEscape(localTunIpv4_) << "\",\n"
        << "  \"tun_prefix\": " << tunPrefix_ << ",\n"
        << "  \"tun_mtu\": " << tunMtu_ << ",\n"
        << "  \"auto_config_ipv4\": " << (autoConfigIpv4_ ? "true" : "false") << ",\n"
        << "  \"keepalive_interval\": " << keepaliveInterval_ << ",\n"
        << "  \"peer_timeout\": " << peerTimeout_ << ",\n"
        << "  \"dummy_traffic_enabled\": " << (dummyTrafficEnabled_ ? "true" : "false") << ",\n"
        << "  \"punch_timeout\": " << punchTimeout_ << ",\n"
        << "  \"nat4_source_port_start\": " << nat4SourcePortStart_ << ",\n"
        << "  \"nat4_source_port_count\": " << nat4SourcePortCount_ << ",\n"
        << "  \"nat4_peer_port_offset\": " << nat4PeerPortOffset_ << ",\n"
        << "  \"nat4_round_timeout\": " << nat4RoundTimeout_ << ",\n"
        << "  \"ipv6_fallback_enabled\": "
        << (ipv6FallbackEnabled_ ? "true" : "false") << ",\n"
        << "  \"ipv6_accept_inbound\": "
        << (ipv6AcceptInbound_ ? "true" : "false") << ",\n"
        << "  \"ipv6_listen_port\": " << ipv6ListenPort_ << ",\n"
        << "  \"ipv6_probe_host\": \"" << JsonEscape(ipv6ProbeHost_) << "\",\n"
        << "  \"ipv6_probe_port\": " << ipv6ProbePort_ << ",\n"
        << "  \"ipv6_fallback_timeout\": " << ipv6FallbackTimeout_ << ",\n"
        << "  \"ipv4_relay_fallback_enabled\": "
        << (ipv4RelayFallbackEnabled_ ? "true" : "false") << ",\n"
        << "  \"log_level\": " << logLevelIdx_ << ",\n"
        << "  \"rendezvous_retry_delay_seconds\": "
        << rendezvousRetryDelaySeconds_ << ",\n"
        << "  \"auto_wait_for_peer\": " << (autoWaitForPeer_ ? "true" : "false") << "\n"
        << "}\n";
    output.flush();
    if (!output.good()) {
        const std::string message = "Failed to write configuration: " + configFilePath_;
        ShowConfigSaveMessage(message, false);
        Log(LogLevel::Error, message);
        return false;
    }
    ShowConfigSaveMessage("Configuration saved: " + configFilePath_, true);
    return true;
}

void GuiApp::ShowConfigSaveMessage(std::string message, bool succeeded) {
    configSaveMessage_ = std::move(message);
    configSaveSucceeded_ = succeeded;
    configSaveMessageExpiresAt_ = std::chrono::steady_clock::now()
        + kConfigSaveMessageDuration;
}

void GuiApp::RenderConfigSaveStatus() {
    if (configSaveMessage_.empty()) return;
    if (std::chrono::steady_clock::now() >= configSaveMessageExpiresAt_) {
        configSaveMessage_.clear();
        return;
    }
    const ImVec4 color = configSaveSucceeded_
        ? ImVec4(0.35f, 0.85f, 0.45f, 1.0f)
        : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
    ImGui::TextColored(color, "%s", configSaveMessage_.c_str());
}
