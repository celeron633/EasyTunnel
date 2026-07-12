#include "gui_app.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include "../log.h"
#include "../nat_protocol.h"
#include "../nat_traversal.h"
#include "../util.h"

namespace {
constexpr float kFormLabelWidth = 155.0f;

void GlfwErrorCallback(int error, const char* description) {
    Log(LogLevel::Error, "GLFW Error " + std::to_string(error) + ": " + description);
}

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
    const std::string label = FormatByteValue(bytes, unit) + "##" + id;
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

GuiApp::~GuiApp() {
    SetLogCallback({});
    Shutdown();
}

bool GuiApp::Init() {
    SetLogCallback([this](LogLevel level, const std::string& message) {
        OnLog(level, message);
    });
    try {
        configFilePath_ = std::filesystem::absolute("EasyTunnel_gui.json").string();
    } catch (...) {
        configFilePath_ = "EasyTunnel_gui.json";
    }
    LoadGuiConfig();
    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit()) return false;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    window_ = glfwCreateWindow(860, 680, "EasyTunnel", nullptr, nullptr);
    if (!window_) {
        glfwTerminate();
        return false;
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 130");
    engine_.SetStateCallback([this](TunnelState state, const std::string& message) {
        OnStateChanged(state, message);
    });
    return true;
}

void GuiApp::Run() {
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        RenderFrame();
        ImGui::Render();
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);
    }
    Disconnect();
}

void GuiApp::Shutdown() {
    if (!window_) return;
    engine_.Stop();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window_);
    glfwTerminate();
    window_ = nullptr;
}

void GuiApp::RenderFrame() {
    UpdateLiveStats();
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - 30));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##MainWindow", nullptr, flags);
    if (ImGui::BeginTabBar("##MainTabs")) {
        if (ImGui::BeginTabItem("Connection")) {
            RenderConnectionTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            RenderSettingsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Log")) {
            RenderLogTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    RenderStatusBar();
}

void GuiApp::RenderConnectionTab() {
    const TunnelState state = currentState_.load();
    const bool active = state == TunnelState::Connecting || state == TunnelState::Connected;
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
    if (!active && ImGui::Button("Refresh clients")) RefreshClients();
    ImGui::SameLine();
    ImGui::TextDisabled("Only clients currently waiting/connecting are listed");

    if (ImGui::BeginListBox("##ClientList", ImVec2(-FLT_MIN, 160.0f))) {
        if (clients_.empty()) ImGui::TextDisabled("No online clients");
        for (int i = 0; i < static_cast<int>(clients_.size()); ++i) {
            const bool selected = selectedClient_ == i;
            if (ImGui::Selectable(clients_[i].c_str(), selected) && !active) {
                selectedClient_ = i;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndListBox();
    }

    ImGui::Spacing();
    if (!active) {
        if (ImGui::Button("Wait for peer", ImVec2(130, 0))) StartConnection("");
        ImGui::SameLine();
        const bool canConnect = selectedClient_ >= 0
            && selectedClient_ < static_cast<int>(clients_.size());
        if (!canConnect) ImGui::BeginDisabled();
        if (ImGui::Button("Connect selected", ImVec2(150, 0)) && canConnect) {
            StartConnection(clients_[selectedClient_]);
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

void GuiApp::RenderSettingsTab() {
    bool configChanged = false;
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
        FormField("Log Level");
        configChanged |= ImGui::Combo("##LogLevel", &logLevelIdx_, kLogLevels, kLogLevelCount);
        EndForm();
    }
    if (configChanged) SaveGuiConfig();
    ImGui::Spacing();
    RenderConfigSaveStatus();
}

void GuiApp::RenderLogTab() {
    ImGui::Spacing();
    if (ImGui::Button("Clear")) {
        std::lock_guard<std::mutex> lock(logMutex_);
        logLines_.clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &logAutoScroll_);
    ImGui::SameLine();
    const std::string filePath = GetLogFilePath();
    ImGui::TextDisabled("File: %s", filePath.empty() ? "unavailable" : filePath.c_str());
    ImGui::Separator();
    ImGui::BeginChild("##LogLines", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        for (const auto& line : logLines_) ImGui::TextUnformatted(line.c_str());
    }
    if (logAutoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
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

void GuiApp::StartConnection(const std::string& targetPeerId) {
    std::string error;
    if (!ValidateCommonFields(&error)) {
        OnStateChanged(TunnelState::Error, error);
        return;
    }
    if (!targetPeerId.empty() && targetPeerId == peerId_) {
        OnStateChanged(TunnelState::Error, "Cannot connect to this client itself");
        return;
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
    cfg.punch_timeout = static_cast<uint16_t>(punchTimeout_);
    TryParseLogLevel(kLogLevels[logLevelIdx_], &cfg.log_level);
    engine_.Start(cfg);
}

void GuiApp::RefreshClients() {
    std::string error;
    if (!ValidateCommonFields(&error)) {
        OnStateChanged(TunnelState::Error, error);
        return;
    }
    std::vector<std::string> clients;
    if (!ListRendezvousClients(serverAddress_, static_cast<uint16_t>(serverPort_),
                               roomId_, authToken_, &clients, &error)) {
        OnStateChanged(TunnelState::Error, error);
        return;
    }
    clients.erase(std::remove(clients.begin(), clients.end(), std::string(peerId_)), clients.end());
    std::sort(clients.begin(), clients.end());
    clients_ = std::move(clients);
    selectedClient_ = clients_.empty() ? -1 : 0;
    OnStateChanged(TunnelState::Disconnected,
        clients_.empty() ? "No online clients" : "Client list refreshed");
}

void GuiApp::Disconnect() {
    engine_.Stop();
    OnStateChanged(TunnelState::Disconnected, "Disconnected");
}

bool GuiApp::LoadGuiConfig() {
    std::ifstream input(configFilePath_, std::ios::binary);
    if (!input.is_open()) {
        configSaveSucceeded_ = true;
        configSaveMessage_ = "Configuration will be saved to " + configFilePath_;
        return true;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    const std::string json = contents.str();
    if (json.find('{') == std::string::npos || json.find('}') == std::string::npos) {
        configSaveSucceeded_ = false;
        configSaveMessage_ = "Invalid configuration JSON: " + configFilePath_;
        Log(LogLevel::Error, configSaveMessage_);
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
    JsonInt(json, "punch_timeout", &punchTimeout_);
    JsonInt(json, "log_level", &logLevelIdx_);

    serverPort_ = std::clamp(serverPort_, 1, 65535);
    tunPrefix_ = std::clamp(tunPrefix_, 0, 32);
    tunMtu_ = std::clamp(tunMtu_, 576, 9000);
    keepaliveInterval_ = std::clamp(keepaliveInterval_, 1, 300);
    peerTimeout_ = std::clamp(peerTimeout_, keepaliveInterval_ + 1, 3600);
    punchTimeout_ = std::clamp(punchTimeout_, 1, 600);
    logLevelIdx_ = std::clamp(logLevelIdx_, 0, kLogLevelCount - 1);
    configSaveSucceeded_ = true;
    configSaveMessage_ = "Configuration loaded from " + configFilePath_;
    Log(LogLevel::Info, configSaveMessage_);
    return true;
}

bool GuiApp::SaveGuiConfig() {
    std::ofstream output(configFilePath_, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        configSaveSucceeded_ = false;
        configSaveMessage_ = "Failed to save configuration: " + configFilePath_;
        Log(LogLevel::Error, configSaveMessage_);
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
        << "  \"punch_timeout\": " << punchTimeout_ << ",\n"
        << "  \"log_level\": " << logLevelIdx_ << "\n"
        << "}\n";
    output.flush();
    if (!output.good()) {
        configSaveSucceeded_ = false;
        configSaveMessage_ = "Failed to write configuration: " + configFilePath_;
        Log(LogLevel::Error, configSaveMessage_);
        return false;
    }
    configSaveSucceeded_ = true;
    configSaveMessage_ = "Configuration saved: " + configFilePath_;
    return true;
}

void GuiApp::RenderConfigSaveStatus() {
    if (configSaveMessage_.empty()) return;
    const ImVec4 color = configSaveSucceeded_
        ? ImVec4(0.35f, 0.85f, 0.45f, 1.0f)
        : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
    ImGui::TextColored(color, "%s", configSaveMessage_.c_str());
}

void GuiApp::OnStateChanged(TunnelState state, const std::string& message) {
    currentState_.store(state);
    std::lock_guard<std::mutex> lock(statusMutex_);
    statusMessage_ = message.empty() ? "Disconnected" : message;
}

void GuiApp::OnLog(LogLevel /*level*/, const std::string& message) {
    std::lock_guard<std::mutex> lock(logMutex_);
    logLines_.push_back(message);
    constexpr size_t kMaxLogLines = 2000;
    if (logLines_.size() > kMaxLogLines) {
        logLines_.erase(logLines_.begin(), logLines_.begin() + 500);
    }
}
