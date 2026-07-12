#include "gui_app.h"

#include <cfloat>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include "../log.h"

// ---------------------------------------------------------------------------
// Helper: GLFW error callback
// ---------------------------------------------------------------------------
static void GlfwErrorCallback(int error, const char* description) {
	Log(LogLevel::Error, "GLFW Error " + std::to_string(error) + ": " + description);
}

namespace {

constexpr float kFormLabelWidth = 155.0f;
constexpr float kHistoryComboWidth = 96.0f;

bool BeginForm(const char* id) {
	if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) {
		return false;
	}
	ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, kFormLabelWidth);
	ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthStretch);
	return true;
}

void EndForm() {
	ImGui::EndTable();
}

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

float ButtonWidth(const char* label) {
	const ImGuiStyle& style = ImGui::GetStyle();
	return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
}

float FieldWidthWithTrailing(float trailingWidth) {
	const float spacing = ImGui::GetStyle().ItemSpacing.x;
	const float width = ImGui::GetContentRegionAvail().x - trailingWidth - spacing;
	return width > 120.0f ? width : 120.0f;
}

}  // namespace

// ---------------------------------------------------------------------------
// GuiApp implementation
// ---------------------------------------------------------------------------

GuiApp::GuiApp() : history_("EasyTunnel.ini") {}

GuiApp::~GuiApp() {
	Shutdown();
}

bool GuiApp::Init() {
	glfwSetErrorCallback(GlfwErrorCallback);
	if (!glfwInit()) {
		return false;
	}

	// GL 3.0 + GLSL 130
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

	window_ = glfwCreateWindow(800, 600, "EasyTunnel", nullptr, nullptr);
	if (!window_) {
		glfwTerminate();
		return false;
	}

	glfwMakeContextCurrent(window_);
	glfwSwapInterval(1);  // VSync

	// Setup ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	ImGui::StyleColorsDark();

	// Setup platform/renderer backends
	ImGui_ImplGlfw_InitForOpenGL(window_, true);
	ImGui_ImplOpenGL3_Init("#version 130");

	// Load connection history
	history_.Load();

	// Populate peer input from history
	if (!history_.GetPeers().empty()) {
		std::snprintf(peerAddrInput_, sizeof(peerAddrInput_), "%s",
			history_.GetPeers()[0].c_str());
		selectedPeerHistoryIdx_ = 0;
	}

	// Set local address from history
	RefreshLocalAddresses();
	if (!history_.GetLastLocalIpv6().empty()) {
		std::snprintf(localAddrInput_, sizeof(localAddrInput_), "%s",
			history_.GetLastLocalIpv6().c_str());
		for (size_t i = 0; i < localAddresses_.size(); ++i) {
			if (localAddresses_[i].address == history_.GetLastLocalIpv6()) {
				selectedLocalIdx_ = static_cast<int>(i);
				break;
			}
		}
	}

	// Set engine callback
	engine_.SetStateCallback([this](TunnelState state, const std::string& msg) {
		OnStateChanged(state, msg);
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
		int displayW, displayH;
		glfwGetFramebufferSize(window_, &displayW, &displayH);
		glViewport(0, 0, displayW, displayH);
		glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

		glfwSwapBuffers(window_);
	}

	// Ensure tunnel is stopped on exit
	Disconnect();
}

void GuiApp::Shutdown() {
	if (window_) {
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		glfwDestroyWindow(window_);
		glfwTerminate();
		window_ = nullptr;
	}
}

void GuiApp::RenderFrame() {
	// Main window fills the entire viewport
	ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - 30));

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoBringToFrontOnFocus;

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
		ImGui::EndTabBar();
	}

	ImGui::End();

	// Status bar at the bottom
	RenderStatusBar();
}

void GuiApp::RenderConnectionTab() {
	ImGui::Spacing();

	if (BeginForm("##ConnectionForm")) {
		FormField("Manual Local Address");
		ImGui::Checkbox("##ManualLocalAddress", &manualLocalAddr_);

		FormField("Local Bind Address");
		if (manualLocalAddr_) {
			ImGui::InputText("##LocalAddressManual", localAddrInput_,
				sizeof(localAddrInput_));
		} else {
			const float refreshWidth = ButtonWidth("Refresh");
			ImGui::SetNextItemWidth(FieldWidthWithTrailing(refreshWidth));
			if (!localAddresses_.empty()) {
				std::string preview = localAddresses_[selectedLocalIdx_].address +
					" (" + localAddresses_[selectedLocalIdx_].interface_name + ")";

				if (ImGui::BeginCombo("##LocalIPv6", preview.c_str())) {
					for (int i = 0; i < static_cast<int>(localAddresses_.size()); ++i) {
						std::string label = localAddresses_[i].address +
							" (" + localAddresses_[i].interface_name + ")";
						bool selected = (selectedLocalIdx_ == i);
						if (ImGui::Selectable(label.c_str(), selected)) {
							selectedLocalIdx_ = i;
						}
						if (selected) {
							ImGui::SetItemDefaultFocus();
						}
					}
					ImGui::EndCombo();
				}
			} else {
				char emptyAddress[1] = {};
				ImGui::BeginDisabled();
				ImGui::InputText("##LocalIPv6Empty", emptyAddress,
					sizeof(emptyAddress), ImGuiInputTextFlags_ReadOnly);
				ImGui::EndDisabled();
			}

			ImGui::SameLine();
			if (ImGui::Button("Refresh")) {
				RefreshLocalAddresses();
			}
		}

		FormField("Peer Address");
		const auto& peers = history_.GetPeers();
		if (!peers.empty()) {
			ImGui::SetNextItemWidth(FieldWidthWithTrailing(kHistoryComboWidth));
		}
		ImGui::InputText("##PeerAddress", peerAddrInput_, sizeof(peerAddrInput_));

		if (!peers.empty()) {
			ImGui::SameLine();
			ImGui::SetNextItemWidth(kHistoryComboWidth);
			if (ImGui::BeginCombo("##PeerHistory", "History", ImGuiComboFlags_NoPreview)) {
				for (int i = 0; i < static_cast<int>(peers.size()); ++i) {
					bool selected = (selectedPeerHistoryIdx_ == i);
					if (ImGui::Selectable(peers[i].c_str(), selected)) {
						selectedPeerHistoryIdx_ = i;
						std::snprintf(peerAddrInput_, sizeof(peerAddrInput_),
							"%s", peers[i].c_str());
					}
				}
				ImGui::EndCombo();
			}
		}

		EndForm();
	}

	// Validation hint
	std::string localStr(localAddrInput_);
	if (manualLocalAddr_ && !localStr.empty() && !ValidateIpAddress(localStr)) {
		ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
			"Invalid local bind address format");
	}

	std::string peerStr(peerAddrInput_);
	if (!peerStr.empty() && !ValidateIpAddress(peerStr)) {
		ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Invalid IP address format");
	}

	ImGui::Spacing();
	ImGui::Spacing();
	ImGui::Separator();

	// Connect / Disconnect buttons
	bool isConnected = (currentState_ == TunnelState::Connected ||
		currentState_ == TunnelState::Connecting);

	if (!isConnected) {
		if (ImGui::Button("Connect", ImVec2(120, 0))) {
			Connect();
		}
	} else {
		if (ImGui::Button("Disconnect", ImVec2(120, 0))) {
			Disconnect();
		}
	}

	ImGui::SameLine();
	{
		std::lock_guard<std::mutex> lock(statusMutex_);
		ImVec4 color;
		switch (currentState_) {
			case TunnelState::Connected:
				color = ImVec4(0.2f, 1.0f, 0.2f, 1.0f);
				break;
			case TunnelState::Connecting:
				color = ImVec4(1.0f, 1.0f, 0.2f, 1.0f);
				break;
			case TunnelState::Error:
				color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
				break;
			default:
				color = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
				break;
		}
		ImGui::TextColored(color, "%s", statusMessage_.c_str());
	}

	// Packet statistics
	ImGui::Spacing();
	ImGui::Spacing();
	ImGui::Text("Packet Statistics");
	ImGui::Separator();

	const auto& stats = engine_.GetStats();
	ImGui::Columns(2, "stats_columns", true);
	ImGui::Text("TX Packets: %llu", static_cast<unsigned long long>(stats.txPackets.load()));
	ImGui::Text("TX Bytes: %llu", static_cast<unsigned long long>(stats.txBytes.load()));
	ImGui::NextColumn();
	ImGui::Text("RX Packets: %llu", static_cast<unsigned long long>(stats.rxPackets.load()));
	ImGui::Text("RX Bytes: %llu", static_cast<unsigned long long>(stats.rxBytes.load()));
	ImGui::Columns(1);
}

void GuiApp::RenderSettingsTab() {
	ImGui::Spacing();

	// Network Settings Panel
	ImGui::Text("Network Settings");
	ImGui::Separator();
	ImGui::Spacing();

	if (BeginForm("##NetworkSettingsForm")) {
		FormField("UDP Port");
		ImGui::InputInt("##UdpPort", &udpPort_);
		if (udpPort_ < 1) udpPort_ = 1;
		if (udpPort_ > 65535) udpPort_ = 65535;

		FormField("TUN MTU");
		ImGui::InputInt("##TunMtu", &tunMtu_);
		if (tunMtu_ < 576) tunMtu_ = 576;
		if (tunMtu_ > 9000) tunMtu_ = 9000;
		if (tunMtu_ > 1452) {
			FormMessage(ImVec4(1.0f, 0.8f, 0.0f, 1.0f),
				"Warning: MTU > 1452 may cause UDP fragmentation");
		}

		EndForm();
	}

	ImGui::Spacing();
	ImGui::Spacing();

	// TUN Adapter Settings Panel
	ImGui::Text("TUN Adapter Settings");
	ImGui::Separator();
	ImGui::Spacing();

	if (BeginForm("##TunSettingsForm")) {
		FormField("Adapter Name");
		ImGui::InputText("##AdapterName", adapterName_, sizeof(adapterName_));

		FormField("Tunnel Type");
		ImGui::InputText("##TunnelType", tunnelType_, sizeof(tunnelType_));

		FormField("Local TUN IPv4");
		ImGui::InputText("##LocalTunIpv4", localTunIpv4_, sizeof(localTunIpv4_));

		FormField("TUN Prefix (CIDR)");
		ImGui::SliderInt("##TunPrefix", &tunPrefix_, 0, 32);

		FormField("Auto Configure IPv4");
		ImGui::Checkbox("##AutoConfigureIpv4", &autoConfigIpv4_);

		EndForm();
	}

	ImGui::Spacing();
	ImGui::Spacing();

	// Logging Settings Panel
	ImGui::Text("Logging");
	ImGui::Separator();
	ImGui::Spacing();

	if (BeginForm("##LoggingForm")) {
		FormField("Log Level");
		ImGui::Combo("##LogLevel", &logLevelIdx_, GuiApp::kLogLevels, GuiApp::kLogLevelCount);
		EndForm();
	}
}

void GuiApp::RenderStatusBar() {
	ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x,
		viewport->WorkPos.y + viewport->WorkSize.y - 30));
	ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, 30));

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;

	ImGui::Begin("##StatusBar", nullptr, flags);

	const auto& stats = engine_.GetStats();
	ImGui::Text("TX: %llu pkts | RX: %llu pkts",
		static_cast<unsigned long long>(stats.txPackets.load()),
		static_cast<unsigned long long>(stats.rxPackets.load()));

	ImGui::SameLine(ImGui::GetWindowWidth() - 200);
	{
		std::lock_guard<std::mutex> lock(statusMutex_);
		ImGui::Text("Status: %s", statusMessage_.c_str());
	}

	ImGui::End();
}

void GuiApp::Connect() {
	std::string peerStr(peerAddrInput_);
	if (peerStr.empty()) {
		std::lock_guard<std::mutex> lock(statusMutex_);
		statusMessage_ = "Error: Peer address is required";
		currentState_ = TunnelState::Error;
		return;
	}

	UdpEndpoint peerEndpoint{};
	if (!ParseUdpEndpoint(peerStr, static_cast<uint16_t>(udpPort_), &peerEndpoint)) {
		std::lock_guard<std::mutex> lock(statusMutex_);
		statusMessage_ = "Error: Invalid peer address";
		currentState_ = TunnelState::Error;
		return;
	}

	UdpEndpoint localEndpoint{};
	std::string localStr(localAddrInput_);
	if (manualLocalAddr_) {
		if (localStr.empty()) {
			std::lock_guard<std::mutex> lock(statusMutex_);
			statusMessage_ = "Error: Local bind address is required";
			currentState_ = TunnelState::Error;
			return;
		}
		if (!ParseUdpEndpoint(localStr, static_cast<uint16_t>(udpPort_), &localEndpoint)) {
			std::lock_guard<std::mutex> lock(statusMutex_);
			statusMessage_ = "Error: Invalid local bind address";
			currentState_ = TunnelState::Error;
			return;
		}
	}

	// Build config from UI settings
	Config cfg;
	cfg.local_addr = manualLocalAddr_ ? localStr : ((peerEndpoint.family == AF_INET)
		? "0.0.0.0"
		: (localAddresses_.empty() ? "::" : localAddresses_[selectedLocalIdx_].address));
	cfg.peer_addr = peerStr;
	cfg.udp_port = static_cast<uint16_t>(udpPort_);
	cfg.adapter_name = adapterName_;
	cfg.tunnel_type = tunnelType_;
	cfg.local_tun_ipv4 = localTunIpv4_;
	cfg.tun_prefix = static_cast<uint8_t>(tunPrefix_);
	cfg.tun_mtu = static_cast<uint16_t>(tunMtu_);
	cfg.auto_config_ipv4 = autoConfigIpv4_;

	LogLevel lvl = LogLevel::Info;
	TryParseLogLevel(GuiApp::kLogLevels[logLevelIdx_], &lvl);
	cfg.log_level = lvl;

	// Validate local TUN IPv4
	std::string tunIpv4Str(localTunIpv4_);
	if (tunIpv4Str.empty()) {
		std::lock_guard<std::mutex> lock(statusMutex_);
		statusMessage_ = "Error: Local TUN IPv4 is required";
		currentState_ = TunnelState::Error;
		return;
	}

	// Save to history
	history_.AddPeer(peerStr);
	if (peerEndpoint.family == AF_INET6
		&& (!manualLocalAddr_ || localEndpoint.family == AF_INET6)) {
		history_.SetLastLocalIpv6(cfg.local_addr);
	}
	history_.Save();

	// Start engine
	engine_.Start(cfg);
}

void GuiApp::Disconnect() {
	engine_.Stop();
	std::lock_guard<std::mutex> lock(statusMutex_);
	currentState_ = TunnelState::Disconnected;
	statusMessage_ = "Disconnected";
}

void GuiApp::RefreshLocalAddresses() {
	localAddresses_ = EnumerateLocalIpv6Addresses();
	if (selectedLocalIdx_ >= static_cast<int>(localAddresses_.size())) {
		selectedLocalIdx_ = 0;
	}
}

void GuiApp::OnStateChanged(TunnelState state, const std::string& msg) {
	std::lock_guard<std::mutex> lock(statusMutex_);
	currentState_ = state;
	statusMessage_ = msg;
}
