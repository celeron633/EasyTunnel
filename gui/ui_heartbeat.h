#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

enum class UiPhase : uint8_t {
    Starting,
    PollEvents,
    ProcessAutoWait,
    OpenGLNewFrame,
    GlfwNewFrame,
    ImGuiNewFrame,
    RenderFrame,
    ImGuiRender,
    GetFramebufferSize,
    OpenGLClear,
    RenderDrawData,
    SwapBuffers,
    FrameComplete,
    ShutdownEngine,
    ShutdownUi,
};

// A low-frequency watchdog for diagnosing UI stalls.  It writes directly to a
// dedicated file, bypassing the normal Log callback and the in-memory GUI log.
class UiHeartbeat {
public:
    UiHeartbeat() = default;
    ~UiHeartbeat();

    UiHeartbeat(const UiHeartbeat&) = delete;
    UiHeartbeat& operator=(const UiHeartbeat&) = delete;

    void Start(std::string graphicsInfo);
    void Stop();
    void SetPhase(UiPhase phase);
    void UpdateWindowState(bool visible, int framebufferWidth, int framebufferHeight,
                           unsigned int glError);
    void CompleteFrame();

private:
    void WatchdogThread(std::string graphicsInfo);

    std::atomic<bool> running_{false};
    std::atomic<UiPhase> phase_{UiPhase::Starting};
    std::atomic<int64_t> phaseChangedAtMs_{0};
    std::atomic<int64_t> lastFrameAtMs_{0};
    std::atomic<uint64_t> frameCount_{0};
    std::atomic<bool> windowVisible_{false};
    std::atomic<int> framebufferWidth_{0};
    std::atomic<int> framebufferHeight_{0};
    std::atomic<unsigned int> glError_{0};
    std::mutex waitMutex_;
    std::condition_variable waitCondition_;
    std::thread watchdogThread_;
};
