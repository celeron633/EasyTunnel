#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "../tunnel_engine.h"

struct GLFWwindow;

class GuiApp {
public:
    static constexpr const char* kLogLevels[] = {"Debug", "Info", "Warn", "Error"};
    static constexpr int kLogLevelCount = 4;

    GuiApp() = default;
    ~GuiApp();

    bool Init();
    void Run();
    void Shutdown();

private:
    void RenderFrame();
    void RenderConnectionTab();
    void RenderSettingsTab();
    void RenderLogTab();
    void RenderStatusBar();
    void StartConnection(const std::string& targetPeerId);
    void Disconnect();
    void RefreshClients();
    void OnStateChanged(TunnelState state, const std::string& message);
    void OnLog(LogLevel level, const std::string& message);
    bool ValidateCommonFields(std::string* error) const;
    bool LoadGuiConfig();
    bool SaveGuiConfig();
    void RenderConfigSaveStatus();

    GLFWwindow* window_ = nullptr;
    TunnelEngine engine_;

    char serverAddress_[256] = "127.0.0.1";
    int serverPort_ = 3478;
    char roomId_[128] = "default-room";
    char peerId_[128] = "node-a";
    char authToken_[128] = {};
    std::vector<std::string> clients_;
    int selectedClient_ = -1;

    char adapterName_[128] = "EasyTunnel";
    char localTunIpv4_[64] = "10.66.0.1";
    int tunPrefix_ = 24;
    int tunMtu_ = 1452;
    bool autoConfigIpv4_ = true;
    int keepaliveInterval_ = 15;
    int peerTimeout_ = 45;
    int punchTimeout_ = 30;
    int logLevelIdx_ = 1;

    std::mutex statusMutex_;
    std::string statusMessage_ = "Disconnected";
    std::atomic<TunnelState> currentState_{TunnelState::Disconnected};
    std::mutex logMutex_;
    std::vector<std::string> logLines_;
    bool logAutoScroll_ = true;
    std::string configFilePath_;
    std::string configSaveMessage_;
    bool configSaveSucceeded_ = true;
};
