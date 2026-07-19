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

    bool Init(GLFWwindow* window);
    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
