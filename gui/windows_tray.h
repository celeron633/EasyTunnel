#pragma once

#include <memory>

struct GLFWwindow;

// Owns the Windows notification-area icon and the native minimize/exit behavior.
class WindowsTray {
public:
    WindowsTray();
    ~WindowsTray();

    WindowsTray(const WindowsTray&) = delete;
    WindowsTray& operator=(const WindowsTray&) = delete;

    bool Init(GLFWwindow* window, const bool* closeToMinimize);
    // Changes both the notification-area icon and the GUI window/taskbar icon.
    // unavailable is used for Disconnected, Waiting, and Error states.
    void UpdateStatus(bool unavailable, bool rxActive, bool txActive);
    bool ConsumeExitRequest();
    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
