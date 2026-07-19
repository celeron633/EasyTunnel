#include "ui_heartbeat.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {
constexpr auto kHeartbeatInterval = std::chrono::seconds(5);

int64_t SteadyMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::filesystem::path UiLogPath() {
#ifdef _WIN32
    wchar_t executable[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return std::filesystem::path(executable).parent_path()
            / L"EasyTunnel_gui_ui.log";
    }
#endif
    try {
        return std::filesystem::absolute("EasyTunnel_gui_ui.log");
    } catch (...) {
        return std::filesystem::path("EasyTunnel_gui_ui.log");
    }
}

const char* PhaseName(UiPhase phase) {
    switch (phase) {
        case UiPhase::Starting: return "starting";
        case UiPhase::PollEvents: return "glfwPollEvents";
        case UiPhase::ProcessAutoWait: return "ProcessAutoWait";
        case UiPhase::OpenGLNewFrame: return "ImGui_ImplOpenGL3_NewFrame";
        case UiPhase::GlfwNewFrame: return "ImGui_ImplGlfw_NewFrame";
        case UiPhase::ImGuiNewFrame: return "ImGui::NewFrame";
        case UiPhase::RenderFrame: return "RenderFrame";
        case UiPhase::ImGuiRender: return "ImGui::Render";
        case UiPhase::GetFramebufferSize: return "glfwGetFramebufferSize";
        case UiPhase::OpenGLClear: return "glClear";
        case UiPhase::RenderDrawData: return "ImGui_ImplOpenGL3_RenderDrawData";
        case UiPhase::SwapBuffers: return "glfwSwapBuffers";
        case UiPhase::FrameComplete: return "frame-complete";
        case UiPhase::ShutdownEngine: return "shutdown-engine";
        case UiPhase::ShutdownUi: return "shutdown-ui";
    }
    return "unknown";
}

std::string Timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << '.'
           << std::setfill('0') << std::setw(3) << milliseconds.count();
    return output.str();
}
}  // namespace

UiHeartbeat::~UiHeartbeat() {
    Stop();
}

void UiHeartbeat::Start(std::string graphicsInfo) {
    if (running_.exchange(true)) return;
    const int64_t nowMs = SteadyMilliseconds();
    phase_.store(UiPhase::Starting);
    phaseChangedAtMs_.store(nowMs);
    lastFrameAtMs_.store(nowMs);
    watchdogThread_ = std::thread(
        &UiHeartbeat::WatchdogThread, this, std::move(graphicsInfo));
}

void UiHeartbeat::Stop() {
    if (!running_.exchange(false)) return;
    waitCondition_.notify_all();
    if (watchdogThread_.joinable()) watchdogThread_.join();
}

void UiHeartbeat::SetPhase(UiPhase phase) {
    phase_.store(phase);
    phaseChangedAtMs_.store(SteadyMilliseconds());
}

void UiHeartbeat::UpdateWindowState(bool visible, int framebufferWidth,
                                    int framebufferHeight, unsigned int glError) {
    windowVisible_.store(visible);
    framebufferWidth_.store(framebufferWidth);
    framebufferHeight_.store(framebufferHeight);
    glError_.store(glError);
}

void UiHeartbeat::CompleteFrame() {
    frameCount_.fetch_add(1);
    lastFrameAtMs_.store(SteadyMilliseconds());
    SetPhase(UiPhase::FrameComplete);
}

void UiHeartbeat::WatchdogThread(std::string graphicsInfo) {
    std::ofstream output(UiLogPath(), std::ios::binary | std::ios::app);
    if (!output.is_open()) return;

    output << Timestamp() << " ui-heartbeat start graphics=\"" << graphicsInfo << "\"\n";
    output.flush();

    std::unique_lock<std::mutex> lock(waitMutex_);
    while (running_.load()) {
        if (waitCondition_.wait_for(lock, kHeartbeatInterval,
                                   [this]() { return !running_.load(); })) {
            break;
        }

        const int64_t nowMs = SteadyMilliseconds();
        const int64_t phaseAge = nowMs - phaseChangedAtMs_.load();
        const int64_t frameAge = nowMs - lastFrameAtMs_.load();
        output << Timestamp()
               << " heartbeat phase=" << PhaseName(phase_.load())
               << " phase_age_ms=" << phaseAge
               << " frame=" << frameCount_.load()
               << " frame_age_ms=" << frameAge
               << " visible=" << (windowVisible_.load() ? 1 : 0)
               << " framebuffer=" << framebufferWidth_.load() << 'x'
               << framebufferHeight_.load()
               << " gl_error=0x" << std::hex << glError_.load() << std::dec
               << '\n';
        output.flush();
    }

    output << Timestamp() << " ui-heartbeat stop phase="
           << PhaseName(phase_.load()) << " frame=" << frameCount_.load() << '\n';
    output.flush();
}
