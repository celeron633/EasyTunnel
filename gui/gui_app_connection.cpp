#include "gui_app.h"

#include <algorithm>
#include <cfloat>
#include <iomanip>
#include <sstream>
#include <utility>

#include "imgui.h"

#include "../log.h"
#include "../nat_protocol.h"
#include "../nat_traversal.h"
#include "../util.h"

namespace {
constexpr float kFormLabelWidth = 155.0f;

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

const char* ByteUnitName(int unit) {
    switch (unit) {
        case 1: return "KB";
        case 2: return "MB";
        default: return "Bytes";
    }
}

double ConvertBytes(double bytes, int unit) {
    if (unit == 1) return bytes / 1024.0;
    if (unit == 2) return bytes / (1024.0 * 1024.0);
    return bytes;
}

std::string FormatByteValue(double bytes, int unit) {
    std::ostringstream output;
    if (unit == 0) {
        output << static_cast<unsigned long long>(bytes);
    } else {
        output << std::fixed << std::setprecision(2) << ConvertBytes(bytes, unit);
    }
    return output.str();
}

bool RenderByteValueButton(const char* id, double bytes, int unit, bool perSecond) {
    const std::string label = FormatByteValue(bytes, unit) + "###" + id;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.0f, 1.0f));
    const bool clicked = ImGui::Button(label.c_str());
    ImGui::PopStyleVar();
    ImGui::SameLine(0.0f, 4.0f);
    if (perSecond) {
        const std::string unitLabel = std::string(ByteUnitName(unit)) + "/s";
        ImGui::TextUnformatted(unitLabel.c_str());
    } else {
        ImGui::TextUnformatted(ByteUnitName(unit));
    }
    return clicked;
}
}  // namespace

void GuiApp::RenderConnectionTab() {
    const TunnelState state = currentState_.load();
    const bool active = state == TunnelState::Connecting || state == TunnelState::Connected;
    const bool waiting = active && waitingForPeer_.load();
    const bool canBrowseClients = !active || waiting;
    bool configChanged = false;
    ImGui::Spacing();
    if (active) ImGui::BeginDisabled();
    if (BeginForm("##RendezvousForm")) {
        FormField("Rendezvous Server Addr");
        configChanged |= ImGui::InputText("##ServerAddress", serverAddress_, sizeof(serverAddress_));
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
        EndForm();
    }
    if (active) ImGui::EndDisabled();
    if (configChanged) SaveGuiConfig();
    RenderConfigSaveStatus();

    ImGui::Spacing();
    ImGui::SeparatorText("Online clients in this room");
    if (canBrowseClients && ImGui::Button("Refresh clients")) RefreshClients();
    ImGui::SameLine();
    ImGui::TextDisabled("Only clients currently waiting/connecting are listed");

    if (ImGui::BeginListBox("##ClientList", ImVec2(-FLT_MIN, 160.0f))) {
        if (clients_.empty()) ImGui::TextDisabled("No online clients");
        for (int i = 0; i < static_cast<int>(clients_.size()); ++i) {
            const bool selected = selectedClient_ == i;
            if (ImGui::Selectable(clients_[i].c_str(), selected) && canBrowseClients) {
                selectedClient_ = i;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndListBox();
    }

    ImGui::Spacing();
    const bool canConnect = selectedClient_ >= 0
        && selectedClient_ < static_cast<int>(clients_.size());
    if (!active) {
        if (ImGui::Button("Wait for peer", ImVec2(130, 0))) StartConnection("");
        ImGui::SameLine();
        if (!canConnect) ImGui::BeginDisabled();
        if (ImGui::Button("Connect selected", ImVec2(150, 0)) && canConnect) {
            ConnectSelectedClient();
        }
        if (!canConnect) ImGui::EndDisabled();
    } else if (waiting) {
        if (ImGui::Button("Disconnect", ImVec2(130, 0))) Disconnect();
        ImGui::SameLine();
        if (!canConnect) ImGui::BeginDisabled();
        if (ImGui::Button("Connect selected", ImVec2(150, 0)) && canConnect) {
            ConnectSelectedClient();
        }
        if (!canConnect) ImGui::EndDisabled();
    } else if (ImGui::Button("Disconnect", ImVec2(130, 0))) {
        Disconnect();
    }
    ImGui::SameLine();
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        ImVec4 color(0.7f, 0.7f, 0.7f, 1.0f);
        if (state == TunnelState::Connected) color = ImVec4(0.2f, 1.0f, 0.2f, 1.0f);
        else if (state == TunnelState::Connecting) color = ImVec4(1.0f, 0.85f, 0.2f, 1.0f);
        else if (state == TunnelState::Error) color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
        ImGui::TextColored(color, "%s", statusMessage_.c_str());
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Packet statistics");
    const auto& stats = engine_.GetStats();
    ImGui::Columns(2, "stats", true);
    ImGui::Text("TX Packets: %llu", static_cast<unsigned long long>(stats.txPackets.load()));
    ImGui::TextUnformatted("TX Bytes:");
    ImGui::SameLine();
    if (RenderByteValueButton("TxTotal", static_cast<double>(stats.txBytes.load()),
                              txTotalUnit_, false)) {
        txTotalUnit_ = (txTotalUnit_ + 1) % 3;
    }
    ImGui::TextUnformatted("TX Speed:");
    ImGui::SameLine();
    if (RenderByteValueButton("TxSpeed", txBytesPerSecond_, txSpeedUnit_, true)) {
        txSpeedUnit_ = (txSpeedUnit_ + 1) % 3;
    }
    ImGui::NextColumn();
    ImGui::Text("RX Packets: %llu", static_cast<unsigned long long>(stats.rxPackets.load()));
    ImGui::TextUnformatted("RX Bytes:");
    ImGui::SameLine();
    if (RenderByteValueButton("RxTotal", static_cast<double>(stats.rxBytes.load()),
                              rxTotalUnit_, false)) {
        rxTotalUnit_ = (rxTotalUnit_ + 1) % 3;
    }
    ImGui::TextUnformatted("RX Speed:");
    ImGui::SameLine();
    if (RenderByteValueButton("RxSpeed", rxBytesPerSecond_, rxSpeedUnit_, true)) {
        rxSpeedUnit_ = (rxSpeedUnit_ + 1) % 3;
    }
    ImGui::Columns(1);
}

void GuiApp::UpdateLiveStats() {
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

void GuiApp::RenderStatusBar() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x,
        viewport->WorkPos.y + viewport->WorkSize.y - 30));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, 30));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##StatusBar", nullptr, flags);
    const auto& stats = engine_.GetStats();
    const auto now = std::chrono::steady_clock::now();
    const bool txActive = now - lastTxActivity_ < std::chrono::milliseconds(350);
    const bool rxActive = now - lastRxActivity_ < std::chrono::milliseconds(350);
    const ImVec4 txColor = txActive ? ImVec4(1.0f, 0.12f, 0.12f, 1.0f)
                                     : ImVec4(0.28f, 0.04f, 0.04f, 1.0f);
    const ImVec4 rxColor = rxActive ? ImVec4(0.12f, 1.0f, 0.18f, 1.0f)
                                     : ImVec4(0.04f, 0.28f, 0.06f, 1.0f);
    const ImGuiColorEditFlags ledFlags = ImGuiColorEditFlags_NoTooltip
        | ImGuiColorEditFlags_NoDragDrop | ImGuiColorEditFlags_NoBorder;
    ImGui::ColorButton("##TxLed", txColor, ledFlags, ImVec2(10.0f, 10.0f));
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextUnformatted("TX:");
    ImGui::SameLine(0.0f, 4.0f);
    bool changeStatusUnit = RenderByteValueButton("StatusTx",
        static_cast<double>(stats.txBytes.load()), statusUnit_, false);
    ImGui::SameLine(0.0f, 10.0f);
    ImGui::ColorButton("##RxLed", rxColor, ledFlags, ImVec2(10.0f, 10.0f));
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextUnformatted("RX:");
    ImGui::SameLine(0.0f, 4.0f);
    changeStatusUnit |= RenderByteValueButton("StatusRx",
        static_cast<double>(stats.rxBytes.load()), statusUnit_, false);
    if (changeStatusUnit) statusUnit_ = (statusUnit_ + 1) % 3;

    const float statusX = ImGui::GetWindowWidth() - 330.0f;
    if (ImGui::GetCursorPosX() < statusX) ImGui::SameLine(statusX);
    else ImGui::SameLine();
    std::lock_guard<std::mutex> lock(statusMutex_);
    ImGui::Text("Status: %s", statusMessage_.c_str());
    ImGui::End();
}

bool GuiApp::ValidateCommonFields(std::string* error) const {
    if (serverAddress_[0] == '\0') { *error = "Rendezvous server is required"; return false; }
    if (!IsSafeControlField(roomId_)) { *error = "Invalid room ID"; return false; }
    if (!IsSafeControlField(peerId_)) { *error = "Invalid peer ID"; return false; }
    if (authToken_[0] != '\0' && !IsSafeControlField(authToken_)) {
        *error = "Invalid auth token"; return false;
    }
    in_addr tunAddress{};
    if (!ParseIpv4(localTunIpv4_, &tunAddress)) {
        *error = "Invalid local TUN IPv4"; return false;
    }
    return true;
}

bool GuiApp::StartConnection(const std::string& targetPeerId) {
    std::string error;
    if (!ValidateCommonFields(&error)) {
        OnStateChanged(TunnelState::Error, error);
        return false;
    }
    if (!targetPeerId.empty() && targetPeerId == peerId_) {
        OnStateChanged(TunnelState::Error, "Cannot connect to this client itself");
        return false;
    }
    Config cfg;
    cfg.rendezvous_addr = serverAddress_;
    cfg.rendezvous_port = static_cast<uint16_t>(serverPort_);
    cfg.room_id = roomId_;
    cfg.peer_id = peerId_;
    cfg.target_peer_id = targetPeerId;
    cfg.auth_token = authToken_;
    cfg.adapter_name = adapterName_;
    cfg.local_tun_ipv4 = localTunIpv4_;
    cfg.tun_prefix = static_cast<uint8_t>(tunPrefix_);
    cfg.tun_mtu = static_cast<uint16_t>(tunMtu_);
    cfg.auto_config_ipv4 = autoConfigIpv4_;
    cfg.keepalive_interval = static_cast<uint16_t>(keepaliveInterval_);
    cfg.peer_timeout = static_cast<uint16_t>(peerTimeout_);
    cfg.dummy_traffic_enabled = dummyTrafficEnabled_;
    cfg.punch_timeout = static_cast<uint16_t>(punchTimeout_);
    cfg.nat4_source_port_start = static_cast<uint16_t>(nat4SourcePortStart_);
    cfg.nat4_source_port_count = static_cast<uint16_t>(nat4SourcePortCount_);
    cfg.nat4_peer_port_offset = static_cast<uint16_t>(nat4PeerPortOffset_);
    cfg.nat4_round_timeout = static_cast<uint16_t>(nat4RoundTimeout_);
    TryParseLogLevel(kLogLevels[logLevelIdx_], &cfg.log_level);
    const bool started = engine_.Start(cfg);
    if (started) waitingForPeer_.store(targetPeerId.empty());
    return started;
}

void GuiApp::ConnectSelectedClient() {
    if (selectedClient_ < 0 || selectedClient_ >= static_cast<int>(clients_.size())) return;
    const std::string target = clients_[selectedClient_];
    suppressAutoWait_.store(true);
    autoWaitPending_.store(false);
    engine_.Stop();
    waitingForPeer_.store(false);
    suppressAutoWait_.store(false);
    if (!StartConnection(target) && autoWaitEnabledRuntime_.load()) {
        autoWaitPending_.store(true);
    }
}

void GuiApp::RefreshClients() {
    std::string error;
    if (!ValidateCommonFields(&error)) {
        SetStatusMessage(error);
        return;
    }
    std::vector<std::string> clients;
    if (!ListRendezvousClients(serverAddress_, static_cast<uint16_t>(serverPort_),
                               roomId_, authToken_, &clients, &error)) {
        SetStatusMessage(error);
        return;
    }
    clients.erase(std::remove(clients.begin(), clients.end(), std::string(peerId_)), clients.end());
    std::sort(clients.begin(), clients.end());
    clients_ = std::move(clients);
    selectedClient_ = clients_.empty() ? -1 : 0;
    SetStatusMessage(clients_.empty() ? "No online clients" : "Client list refreshed");
}

void GuiApp::Disconnect() {
    engine_.Stop();
    waitingForPeer_.store(false);
    OnStateChanged(TunnelState::Disconnected, "Disconnected");
}

void GuiApp::ProcessAutoWait() {
    if (shuttingDown_.load() || !autoWaitEnabledRuntime_.load()
        || !autoWaitPending_.load()) return;
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (nowMs < autoWaitRetryAfterMs_.load()) return;
    const TunnelState state = currentState_.load();
    if (state != TunnelState::Disconnected && state != TunnelState::Error) return;
    std::string error;
    if (!ValidateCommonFields(&error)) {
        OnStateChanged(TunnelState::Error, "Auto wait failed: " + error);
        autoWaitPending_.store(false);
        return;
    }
    if (StartConnection("")) {
        autoWaitPending_.store(false);
    } else {
        autoWaitRetryAfterMs_.store(nowMs + 1000);
    }
}

void GuiApp::OnStateChanged(TunnelState state, const std::string& message) {
    currentState_.store(state);
    if (state == TunnelState::Connected) waitingForPeer_.store(false);
    if (state == TunnelState::Disconnected || state == TunnelState::Error) {
        waitingForPeer_.store(false);
        if (state == TunnelState::Error) {
            const int64_t retryAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count() + 1000;
            autoWaitRetryAfterMs_.store(retryAt);
        } else {
            autoWaitRetryAfterMs_.store(0);
        }
        if (!shuttingDown_.load() && !suppressAutoWait_.load()
            && autoWaitEnabledRuntime_.load()) {
            autoWaitPending_.store(true);
        }
    }
    SetStatusMessage(message.empty() ? "Disconnected" : message);
}

void GuiApp::SetStatusMessage(const std::string& message) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    statusMessage_ = message;
}
