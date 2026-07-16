#include "gui_app.h"

#include <filesystem>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include "disconnect_confirmation_dialog.h"
#include "exit_confirmation_dialog.h"
#include "windows_tray.h"
#endif

#include "../log.h"

namespace {
void GlfwErrorCallback(int error, const char* description) {
    Log(LogLevel::Error, "GLFW Error " + std::to_string(error) + ": " + description);
}
}  // namespace

GuiApp::GuiApp() = default;

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
#ifdef _WIN32
    disconnectConfirmationDialog_ = std::make_unique<DisconnectConfirmationDialog>();
    exitConfirmationDialog_ = std::make_unique<ExitConfirmationDialog>();
    windowsTray_ = std::make_unique<WindowsTray>();
    if (!windowsTray_->Init(
            window_,
            [this]() {
                const TunnelState state = currentState_.load();
                const bool hasActiveConnection = state == TunnelState::Connecting
                    || state == TunnelState::Connected;
                disconnectConfirmationDialog_->Open(hasActiveConnection);
            },
            [this]() { exitConfirmationDialog_->Open(); })) {
        return false;
    }
#endif
    engine_.SetStateCallback([this](TunnelState state, const std::string& message) {
        OnStateChanged(state, message);
    });
    autoWaitEnabledRuntime_.store(autoWaitForPeer_);
    autoWaitPending_.store(autoWaitForPeer_);
    return true;
}

void GuiApp::Run() {
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        ProcessAutoWait();
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
    shuttingDown_.store(true);
    autoWaitPending_.store(false);
    Disconnect();
}

void GuiApp::Shutdown() {
    shuttingDown_.store(true);
    autoWaitPending_.store(false);
    autoWaitEnabledRuntime_.store(false);
    if (!window_) return;
    engine_.Stop();
#ifdef _WIN32
    if (windowsTray_) {
        windowsTray_->Shutdown();
        windowsTray_.reset();
    }
    disconnectConfirmationDialog_.reset();
    exitConfirmationDialog_.reset();
#endif
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
#ifdef _WIN32
    if (disconnectConfirmationDialog_
        && disconnectConfirmationDialog_->Render()
            == DisconnectConfirmationDialog::Result::Confirmed) {
        Disconnect();
    }
    if (exitConfirmationDialog_ && exitConfirmationDialog_->Render()) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }
#endif
}
