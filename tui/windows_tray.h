#pragma once

#ifdef _WIN32

#include <functional>
#include <memory>

// Notification-area icon for the terminal client. The console window belongs
// to the terminal host process, so unlike the GUI tray this one cannot
// intercept minimize or close; it offers show/hide toggling of the hosting
// terminal window and a safe exit through the FTXUI loop.
class TuiWindowsTray {
public:
    TuiWindowsTray();
    ~TuiWindowsTray();

    TuiWindowsTray(const TuiWindowsTray&) = delete;
    TuiWindowsTray& operator=(const TuiWindowsTray&) = delete;

    // exitRequested is invoked from the tray thread and must be thread-safe;
    // ScreenInteractive::ExitLoopClosure() qualifies.
    bool Init(std::function<void()> exitRequested);
    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif  // _WIN32
