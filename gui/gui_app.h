#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../config.h"
#include "../connection_history.h"
#include "../ipv6_utils.h"
#include "../tunnel_engine.h"

// Forward declarations for ImGui/GLFW
struct GLFWwindow;

// ---------------------------------------------------------------------------
// GuiApp: ImGui-based GUI application for EasyTunnel
// ---------------------------------------------------------------------------
class GuiApp {
public:
	static constexpr const char* kLogLevels[] = {"Debug", "Info", "Warn", "Error"};
	static constexpr int kLogLevelCount = 4;

	GuiApp();
	~GuiApp();

	// Initialize window and ImGui context. Returns false on failure.
	bool Init();

	// Run the main loop. Returns when window is closed.
	void Run();

	// Cleanup resources
	void Shutdown();

private:
	// Rendering
	void RenderFrame();
	void RenderConnectionTab();
	void RenderSettingsTab();
	void RenderStatusBar();

	// Actions
	void Connect();
	void Disconnect();
	void RefreshLocalAddresses();

	// State callback from engine
	void OnStateChanged(TunnelState state, const std::string& msg);

	// Window
	GLFWwindow* window_ = nullptr;

	// Engine
	TunnelEngine engine_;

	// Connection history
	ConnectionHistory history_;

	// Local IPv6 addresses
	std::vector<Ipv6AddrInfo> localAddresses_;
	int selectedLocalIdx_ = 0;
	char localAddrInput_[256] = {};
	bool manualLocalAddr_ = false;

	// Peer IP input
	char peerAddrInput_[256] = {};
	int selectedPeerHistoryIdx_ = -1;

	// Status
	std::mutex statusMutex_;
	std::string statusMessage_ = "Disconnected";
	TunnelState currentState_ = TunnelState::Disconnected;

	// Settings (mirroring Config fields)
	int udpPort_ = 44556;
	char adapterName_[128] = "EasyTunnel";
	char tunnelType_[128] = "EasyTunnel";
	char localTunIpv4_[64] = "10.66.0.1";
	int tunPrefix_ = 24;
	int tunMtu_ = 1452;
	bool autoConfigIpv4_ = true;
	int logLevelIdx_ = 1;  // Info
};
