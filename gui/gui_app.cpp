#include "gui_app.h"

#include <filesystem>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include "ui_heartbeat.h"

#ifdef _WIN32
#include "windows_tray.h"
#endif

#include "../log.h"

namespace {
void GlfwErrorCallback(int error, const char* description) {
    Log(LogLevel::Error, "GLFW Error " + std::to_string(error) + ": " + description);
}

std::string OpenGLInfo() {
    const auto* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    return std::string("vendor=") + (vendor ? vendor : "unknown")
        + "; renderer=" + (renderer ? renderer : "unknown")
        + "; version=" + (version ? version : "unknown");
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
    uiHeartbeat_ = std::make_unique<UiHeartbeat>();
    uiHeartbeat_->Start(OpenGLInfo());
#ifdef _WIN32
    windowsTray_ = std::make_unique<WindowsTray>();
    if (!windowsTray_->Init(window_)) {
        return false;
    }
#endif
    engine_.SetStateCallback([this](TunnelState state, const std::string& message) {
        OnStateChanged(state, message);
    });
    autoWaitEnabledRuntime_.store(autoWaitForPeer_);
    autoWaitRetryDelaySecondsRuntime_.store(rendezvousRetryDelaySeconds_);
    autoWaitPending_.store(autoWaitForPeer_);
    return true;
}

void GuiApp::Run() {
    while (!glfwWindowShouldClose(window_)) {
        uiHeartbeat_->SetPhase(UiPhase::PollEvents);
        glfwPollEvents();
        uiHeartbeat_->SetPhase(UiPhase::ProcessAutoWait);
        ProcessAutoWait();
        uiHeartbeat_->SetPhase(UiPhase::OpenGLNewFrame);
        ImGui_ImplOpenGL3_NewFrame();
        uiHeartbeat_->SetPhase(UiPhase::GlfwNewFrame);
        ImGui_ImplGlfw_NewFrame();
        uiHeartbeat_->SetPhase(UiPhase::ImGuiNewFrame);
        ImGui::NewFrame();
        uiHeartbeat_->SetPhase(UiPhase::RenderFrame);
        RenderFrame();
        uiHeartbeat_->SetPhase(UiPhase::ImGuiRender);
        ImGui::Render();
        int width = 0;
        int height = 0;
        uiHeartbeat_->SetPhase(UiPhase::GetFramebufferSize);
        glfwGetFramebufferSize(window_, &width, &height);
        uiHeartbeat_->SetPhase(UiPhase::OpenGLClear);
        glViewport(0, 0, width, height);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        uiHeartbeat_->SetPhase(UiPhase::RenderDrawData);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        const unsigned int glError = glGetError();
        uiHeartbeat_->UpdateWindowState(
            glfwGetWindowAttrib(window_, GLFW_VISIBLE) == GLFW_TRUE,
            width, height, glError);
        uiHeartbeat_->SetPhase(UiPhase::SwapBuffers);
        glfwSwapBuffers(window_);
        uiHeartbeat_->CompleteFrame();
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
    if (uiHeartbeat_) uiHeartbeat_->SetPhase(UiPhase::ShutdownEngine);
    engine_.Stop();
#ifdef _WIN32
    if (windowsTray_) {
        windowsTray_->Shutdown();
        windowsTray_.reset();
    }
#endif
    if (uiHeartbeat_) uiHeartbeat_->SetPhase(UiPhase::ShutdownUi);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window_);
    glfwTerminate();
    window_ = nullptr;
    if (uiHeartbeat_) {
        uiHeartbeat_->Stop();
        uiHeartbeat_.reset();
    }
}

void GuiApp::RenderFrame() {
    UpdateLiveStats();
    UpdateStatisticsHistory();
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
