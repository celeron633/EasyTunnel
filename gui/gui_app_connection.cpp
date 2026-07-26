#include "gui_app.h"

#include <algorithm>
#include <cfloat>
#include <iomanip>
#include <sstream>
#include <utility>

#include "imgui.h"

#include "../rendezvous_client.h"
#include "gui_theme.h"

namespace {
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

// Value plus unit in one flat cell. It has to read as a number, not a button,
// so the frame stays transparent until the pointer is over it.
bool RenderByteValueButton(const char* id, double bytes, int unit, bool perSecond) {
    std::string label = FormatByteValue(bytes, unit) + ' ' + ByteUnitName(unit);
    if (perSecond) label += "/s";
    label += "###";
    label += id;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const bool clicked = ImGui::Button(label.c_str());
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to switch unit");
    return clicked;
}

std::string FormatIdleTime(uint64_t seconds) {
    if (seconds < 60) return std::to_string(seconds) + "s";
    if (seconds < 3600) {
        return std::to_string(seconds / 60) + "m " + std::to_string(seconds % 60) + "s";
    }
    return std::to_string(seconds / 3600) + "h " + std::to_string((seconds % 3600) / 60) + "m";
}

// Shows a value in a table cell, greying out the placeholder used when the
// rendezvous server did not report the field.
void RenderPeerCell(const std::string& value) {
    if (value.empty()) ImGui::TextDisabled("--");
    else ImGui::TextUnformatted(value.c_str());
}
}  // namespace

void GuiApp::RenderConnectionTab() {
    const TunnelState state = currentState_.load();
    const bool active = IsTunnelActive(state);
    const bool waiting = active && waitingForPeer_.load();
    const bool canBrowseClients = !active || waiting;
    const bool canConnect = selectedClient_ >= 0
        && selectedClient_ < static_cast<int>(clients_.size());

    ImGui::Spacing();
    ImGui::SeparatorText("Online peers");
    // The whole action toolbar lives on one row above the list, the way the
    // terminal client lays it out.
    if (!canBrowseClients) ImGui::BeginDisabled();
    if (ImGui::Button("Refresh", ImVec2(90, 0))) RefreshClients();
    if (!canBrowseClients) ImGui::EndDisabled();
    ImGui::SameLine();
    if (active) {
        if (ImGui::Button("Disconnect", ImVec2(120, 0))) Disconnect();
    } else if (ImGui::Button("Wait for peer", ImVec2(120, 0))) {
        StartConnection("");
    }
    ImGui::SameLine();
    if (!canConnect || (active && !waiting)) ImGui::BeginDisabled();
    if (ImGui::Button("Connect selected", ImVec2(150, 0)) && canConnect) {
        ConnectSelectedClient();
    }
    if (!canConnect || (active && !waiting)) ImGui::EndDisabled();
    ImGui::SameLine();
    const std::string onlineLabel = std::to_string(clients_.size()) + " online";
    gui_theme::TextRightAligned(onlineLabel.c_str());

    // The list grows with the room instead of always reserving room for six
    // peers, so an empty room leaves the space to the charts below.
    const int visibleRows = std::clamp(static_cast<int>(clients_.size()), 1, 6);
    const float tableHeight = ImGui::GetTextLineHeightWithSpacing() * (visibleRows + 1)
        + ImGui::GetStyle().CellPadding.y * 2.0f;
    const ImGuiTableFlags clientTableFlags = ImGuiTableFlags_Borders
        | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
        | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##ClientList", 5, clientTableFlags,
                          ImVec2(-FLT_MIN, tableHeight))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Peer ID", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Public endpoint", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("Capabilities", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn("TUN IP", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Idle", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableHeadersRow();
        if (clients_.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("No online peers. Press Refresh.");
        }
        for (int i = 0; i < static_cast<int>(clients_.size()); ++i) {
            const RendezvousPeerInfo& client = clients_[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(i);
            const bool selected = selectedClient_ == i;
            if (ImGui::Selectable(client.peerId.c_str(), selected,
                                  ImGuiSelectableFlags_SpanAllColumns)
                && canBrowseClients) {
                selectedClient_ = i;
            }
            if (selected) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            RenderPeerCell(client.endpoint);
            ImGui::TableSetColumnIndex(2);
            RenderPeerCell(SerializeTraversalModeSequence(client.capabilities));
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", FormatPeerCapabilities(client.capabilities).c_str());
            }
            ImGui::TableSetColumnIndex(3);
            RenderPeerCell(client.tunIp);
            ImGui::TableSetColumnIndex(4);
            RenderPeerCell(FormatIdleTime(client.idleSeconds));
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::SeparatorText("Traffic");
    const auto& stats = engine_.GetStats();
    const auto now = std::chrono::steady_clock::now();
    const bool txActive = now - lastTxActivity_ < std::chrono::milliseconds(350);
    const bool rxActive = now - lastRxActivity_ < std::chrono::milliseconds(350);
    // One row per direction keeps packets, totals and speed under shared
    // headers instead of repeating the labels six times.
    auto trafficRow = [this](const char* name, bool activeDirection,
                             const ImVec4& color, uint64_t packets, double bytes,
                             double bytesPerSecond, const char* totalId,
                             const char* speedId) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        gui_theme::ActivityDot(activeDirection, color);
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextUnformatted(name);
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%llu", static_cast<unsigned long long>(packets));
        ImGui::TableSetColumnIndex(2);
        if (RenderByteValueButton(totalId, bytes, statisticsTotalUnit_, false)) {
            statisticsTotalUnit_ = (statisticsTotalUnit_ + 1) % 3;
        }
        ImGui::TableSetColumnIndex(3);
        if (RenderByteValueButton(speedId, bytesPerSecond, statisticsSpeedUnit_, true)) {
            statisticsSpeedUnit_ = (statisticsSpeedUnit_ + 1) % 3;
        }
    };
    if (ImGui::BeginTable("##Traffic", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Packets", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        trafficRow("TX", txActive, gui_theme::kTx, stats.txPackets.load(),
                   static_cast<double>(stats.txBytes.load()), txBytesPerSecond_,
                   "TxTotal", "TxSpeed");
        trafficRow("RX", rxActive, gui_theme::kRx, stats.rxPackets.load(),
                   static_cast<double>(stats.rxBytes.load()), rxBytesPerSecond_,
                   "RxTotal", "RxSpeed");
        ImGui::EndTable();
    }
    const int64_t rttMilliseconds = stats.rttMilliseconds.load();
    if (rttMilliseconds < 0) ImGui::TextUnformatted("Latency  -- ms");
    else ImGui::Text("Latency  %lld ms", static_cast<long long>(rttMilliseconds));
    RenderStatisticsCharts();
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
    // The TX/RX counters used to be mirrored here from the Connection tab, with
    // a third independent unit toggle. The traffic table owns them now, so the
    // bar carries only what no tab shows: the live state message.
    ImGui::Begin("##StatusBar", nullptr, flags);
    const gui_theme::StateStyle style = gui_theme::StyleFor(currentState_.load());
    std::lock_guard<std::mutex> lock(statusMutex_);
    ImGui::TextColored(style.color, "%s", statusMessage_.c_str());
    ImGui::End();
}

bool GuiApp::StartConnection(const std::string& targetPeerId) {
    std::string error;
    if (!ValidateClientConfig(config_, &error)) {
        OnStateChanged(TunnelState::Error, error);
        return false;
    }
    if (!targetPeerId.empty() && targetPeerId == config_.peerId) {
        OnStateChanged(TunnelState::Error, "Cannot connect to this client itself");
        return false;
    }
    const bool started = engine_.Start(ToEngineConfig(config_, targetPeerId));
    if (started) waitingForPeer_.store(targetPeerId.empty());
    return started;
}

void GuiApp::ConnectSelectedClient() {
    if (selectedClient_ < 0 || selectedClient_ >= static_cast<int>(clients_.size())) return;
    const std::string target = clients_[selectedClient_].peerId;
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
    if (!ValidateClientConfig(config_, &error)) {
        SetStatusMessage(error);
        return;
    }
    std::vector<RendezvousPeerInfo> clients;
    if (!ListRendezvousClients(config_.rendezvousAddress,
                               static_cast<uint16_t>(config_.rendezvousPort),
                               config_.roomId, config_.authToken, &clients, &error)) {
        SetStatusMessage(error);
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
    if (!ValidateClientConfig(config_, &error)) {
        OnStateChanged(TunnelState::Error, "Auto wait failed: " + error);
        autoWaitPending_.store(false);
        return;
    }
    if (StartConnection("")) {
        autoWaitPending_.store(false);
    } else {
        autoWaitRetryAfterMs_.store(nowMs
            + static_cast<int64_t>(autoWaitRetryDelaySecondsRuntime_.load()) * 1000);
    }
}

void GuiApp::OnStateChanged(TunnelState state, const std::string& message) {
    currentState_.store(state);
    if (state == TunnelState::Connected) waitingForPeer_.store(false);
    if (state == TunnelState::Disconnected || state == TunnelState::Error) {
        waitingForPeer_.store(false);
        const int64_t retryAt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()
            + static_cast<int64_t>(autoWaitRetryDelaySecondsRuntime_.load()) * 1000;
        autoWaitRetryAfterMs_.store(retryAt);
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
