#pragma once

#include <functional>
#include <memory>

struct GLFWwindow;

// Owns the Windows notification-area icon and the native window behavior that
// hides the GUI when the user closes or minimizes it.
class WindowsTray {
public:
    WindowsTray();
    ~WindowsTray();

    WindowsTray(const WindowsTray&) = delete;
    WindowsTray& operator=(const WindowsTray&) = delete;

    bool Init(GLFWwindow* window,
              std::function<void()> disconnectCallback,
              std::function<void()> exitCallback);
    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
