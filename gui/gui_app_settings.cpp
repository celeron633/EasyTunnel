#include "gui_app.h"

#include <algorithm>
#include <cfloat>
#include <utility>

#include "imgui.h"
#include "imgui_stdlib.h"

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
}  // namespace

// Two balanced columns: session and timing settings on the left, the data path
// on the right. The whole page then fits the default window without scrolling.
void GuiApp::RenderSettingsTab() {
    bool configChanged = false;
    ImGui::Spacing();
    if (!ImGui::BeginTable("##SettingsColumns", 2,
                           ImGuiTableFlags_SizingStretchSame
                               | ImGuiTableFlags_BordersInnerV)) {
        return;
    }
    ImGui::TableNextColumn();
    ImGui::SeparatorText("Rendezvous");
    if (BeginForm("##RendezvousSettings")) {
        const TunnelState state = currentState_.load();
        const bool active = state == TunnelState::Connecting || state == TunnelState::Connected;
        if (active) ImGui::BeginDisabled();
        FormField("Server address");
        configChanged |= ImGui::InputText("##ServerAddress", &config_.rendezvousAddress);
        FormField("Server port");
        configChanged |= ImGui::InputInt("##ServerPort", &config_.rendezvousPort);
        config_.rendezvousPort = std::clamp(config_.rendezvousPort, 1, 65535);
        FormField("Room ID");
        configChanged |= ImGui::InputText("##RoomId", &config_.roomId);
        FormField("My peer ID");
        configChanged |= ImGui::InputText("##PeerId", &config_.peerId);
        FormField("Auth token");
        configChanged |= ImGui::InputText("##AuthToken", &config_.authToken,
                                          ImGuiInputTextFlags_Password);
        if (active) ImGui::EndDisabled();
        FormField("Retry delay (s)");
        const bool retryDelayChanged = ImGui::InputInt(
            "##RendezvousRetryDelay", &config_.rendezvousRetryDelaySeconds);
        config_.rendezvousRetryDelaySeconds = std::clamp(
            config_.rendezvousRetryDelaySeconds, 1, 3600);
        if (retryDelayChanged) {
            autoWaitRetryDelaySecondsRuntime_.store(config_.rendezvousRetryDelaySeconds);
        }
        configChanged |= retryDelayChanged;
        FormField("Auto wait for peer");
        const bool autoWaitChanged = ImGui::Checkbox("##AutoWaitForPeer",
                                                     &config_.autoWaitForPeer);
        configChanged |= autoWaitChanged;
        if (autoWaitChanged) {
            autoWaitEnabledRuntime_.store(config_.autoWaitForPeer);
            if (config_.autoWaitForPeer) {
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
    ImGui::SeparatorText("NAT liveness");
    if (BeginForm("##NatSettings")) {
        FormField("Keepalive (s)");
        configChanged |= ImGui::InputInt("##Keepalive", &config_.keepaliveInterval);
        config_.keepaliveInterval = std::clamp(config_.keepaliveInterval, 1, 300);
        FormField("Peer timeout (s)");
        configChanged |= ImGui::InputInt("##PeerTimeout", &config_.peerTimeout);
        config_.peerTimeout = std::clamp(config_.peerTimeout,
                                         config_.keepaliveInterval + 1, 3600);
        FormField("Punch timeout (s)");
        configChanged |= ImGui::InputInt("##PunchTimeout", &config_.punchTimeout);
        config_.punchTimeout = std::clamp(config_.punchTimeout, 1, 600);
        FormField("NAT4 port start");
        configChanged |= ImGui::InputInt("##Nat4SourcePortStart",
                                         &config_.nat4SourcePortStart);
        config_.nat4SourcePortStart = std::clamp(config_.nat4SourcePortStart, 1, 65535);
        FormField("NAT4 port count");
        configChanged |= ImGui::InputInt("##Nat4SourcePortCount",
                                         &config_.nat4SourcePortCount);
        config_.nat4SourcePortCount = std::clamp(config_.nat4SourcePortCount, 1, 60);
        config_.nat4SourcePortStart = (std::min)(
            config_.nat4SourcePortStart, 65536 - config_.nat4SourcePortCount);
        FormField("NAT4 peer offset");
        configChanged |= ImGui::InputInt("##Nat4PeerPortOffset",
                                         &config_.nat4PeerPortOffset);
        config_.nat4PeerPortOffset = std::clamp(config_.nat4PeerPortOffset, 0, 256);
        FormField("NAT4 round timeout (s)");
        configChanged |= ImGui::InputInt("##Nat4RoundTimeout", &config_.nat4RoundTimeout);
        config_.nat4RoundTimeout = std::clamp(config_.nat4RoundTimeout, 1, 60);
        EndForm();
    }
    ImGui::Spacing();
    ImGui::SeparatorText("Log and misc");
    if (BeginForm("##LogSettings")) {
        FormField("Log level");
        configChanged |= ImGui::Combo("##LogLevel", &config_.logLevel,
                                      kLogLevels, kLogLevelCount);
        FormField("1 KiB/s dummy traffic");
        configChanged |= ImGui::Checkbox("##DummyTraffic", &config_.dummyTrafficEnabled);
        EndForm();
    }

    ImGui::TableNextColumn();
    ImGui::SeparatorText("TUN adapter");
    if (BeginForm("##TunSettings")) {
        FormField("Adapter name");
        configChanged |= ImGui::InputText("##AdapterName", &config_.adapterName);
        FormField("Local TUN IPv4");
        configChanged |= ImGui::InputText("##TunIpv4", &config_.localTunIpv4);
        FormField("TUN prefix");
        configChanged |= ImGui::SliderInt("##TunPrefix", &config_.tunPrefix, 0, 32);
        FormField("TUN MTU");
        configChanged |= ImGui::InputInt("##TunMtu", &config_.tunMtu);
        config_.tunMtu = std::clamp(config_.tunMtu, 576, 9000);
        if (config_.tunMtu > 1472) {
            FormMessage(ImVec4(1.0f, 0.8f, 0.0f, 1.0f),
                        "MTU > 1472 may cause outer IPv4 fragmentation");
        }
        FormField("Auto configure IPv4");
        configChanged |= ImGui::Checkbox("##AutoConfig", &config_.autoConfigIpv4);
        EndForm();
    }
    ImGui::Spacing();
    ImGui::SeparatorText("Traversal strategy");
    if (ImGui::BeginTable("##TraversalModes", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed, 65.0f);
        ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed, 95.0f);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < config_.traversalModes.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            configChanged |= ImGui::Checkbox("##Enabled",
                                             &config_.traversalModes[i].enabled);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d", static_cast<int>(i + 1));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(TraversalModeDisplayName(config_.traversalModes[i].mode));
            ImGui::TableSetColumnIndex(3);
            if (i == 0) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Up")) {
                std::swap(config_.traversalModes[i], config_.traversalModes[i - 1]);
                configChanged = true;
            }
            if (i == 0) ImGui::EndDisabled();
            ImGui::SameLine();
            if (i + 1 == config_.traversalModes.size()) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Down")) {
                std::swap(config_.traversalModes[i], config_.traversalModes[i + 1]);
                configChanged = true;
            }
            if (i + 1 == config_.traversalModes.size()) ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::SeparatorText("IPv6 direct connection");
    if (BeginForm("##Ipv6FallbackSettings")) {
        FormField("Accept inbound UDP");
        configChanged |= ImGui::Checkbox("##Ipv6Inbound", &config_.ipv6AcceptInbound);
        FormField("Listen port (0=auto)");
        configChanged |= ImGui::InputInt("##Ipv6ListenPort", &config_.ipv6ListenPort);
        config_.ipv6ListenPort = std::clamp(config_.ipv6ListenPort, 0, 65535);
        FormField("Probe host");
        configChanged |= ImGui::InputText("##Ipv6ProbeHost", &config_.ipv6ProbeHost);
        FormField("Probe TCP port");
        configChanged |= ImGui::InputInt("##Ipv6ProbePort", &config_.ipv6ProbePort);
        config_.ipv6ProbePort = std::clamp(config_.ipv6ProbePort, 1, 65535);
        FormField("Probe timeout (s)");
        configChanged |= ImGui::InputInt("##Ipv6FallbackTimeout",
                                         &config_.ipv6FallbackTimeout);
        config_.ipv6FallbackTimeout = std::clamp(config_.ipv6FallbackTimeout, 1, 120);
        EndForm();
    }
    ImGui::EndTable();

    if (configChanged) SaveGuiConfig();
    ImGui::Spacing();
    RenderConfigSaveStatus();
}

bool GuiApp::LoadGuiConfig() {
    bool existed = false;
    std::string error;
    if (!LoadClientConfig(configFilePath_, &config_, &existed, &error)) {
        ShowConfigSaveMessage(error, false);
        Log(LogLevel::Error, error);
        return false;
    }
    const std::string message = existed
        ? "Configuration loaded from " + configFilePath_
        : "Configuration will be saved to " + configFilePath_;
    ShowConfigSaveMessage(message, true);
    if (existed) Log(LogLevel::Info, message);
    return true;
}

bool GuiApp::SaveGuiConfig() {
    std::string error;
    if (!SaveClientConfig(configFilePath_, config_, &error)) {
        ShowConfigSaveMessage(error, false);
        Log(LogLevel::Error, error);
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
