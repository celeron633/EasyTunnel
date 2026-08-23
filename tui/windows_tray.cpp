#ifdef _WIN32

#include "windows_tray.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cwchar>
#include <future>
#include <thread>
#include <utility>

#include "../log.h"
#include "../res/resource.h"

namespace {
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kUpdateIconMessage = WM_APP + 2;
constexpr UINT kToggleCommand = 1001;
constexpr UINT kExitCommand = 1002;
constexpr wchar_t kWindowClass[] = L"EasyTunnelTuiTray";

enum class IconMode : std::size_t {
    Idle,
    Disconnected,
    Rx,
    Tx,
    RxTx,
    Count,
};

constexpr std::array<int, static_cast<std::size_t>(IconMode::Count)> kIconResources{
    IDI_EASYTUNNEL,
    IDI_EASYTUNNEL_DISCONNECTED,
    IDI_EASYTUNNEL_RX,
    IDI_EASYTUNNEL_TX,
    IDI_EASYTUNNEL_RX_TX,
};

IconMode StatusMode(bool unavailable, bool rxActive, bool txActive) {
    if (unavailable) return IconMode::Disconnected;
    if (rxActive && txActive) return IconMode::RxTx;
    if (rxActive) return IconMode::Rx;
    if (txActive) return IconMode::Tx;
    return IconMode::Idle;
}

const wchar_t* StatusTip(IconMode mode) {
    if (mode == IconMode::Disconnected) {
        return L"EasyTunnel - disconnected / waiting";
    }
    return L"EasyTunnel - connected";
}

// Under classic conhost the console window is ours to show and hide. Under
// Windows Terminal GetConsoleWindow() returns the hidden ConPTY pseudo window,
// so fall back to the window that had focus while we started up - the terminal
// that launched the TUI.
HWND ResolveTerminalWindow() {
    HWND console = GetConsoleWindow();
    if (console != nullptr && IsWindowVisible(console)) return console;
    HWND foreground = GetForegroundWindow();
    return foreground != nullptr ? foreground : console;
}
}  // namespace

struct TuiWindowsTray::Impl {
    std::function<void()> exitRequested;
    HWND terminal = nullptr;
    std::atomic<HWND> window{nullptr};
    NOTIFYICONDATAW iconData{};
    std::array<HICON, static_cast<std::size_t>(IconMode::Count)> icons{};
    HICON fallbackIcon = nullptr;
    std::atomic<IconMode> requestedMode{IconMode::Disconnected};
    IconMode currentMode = IconMode::Disconnected;
    UINT taskbarCreatedMessage = 0;
    std::thread thread;

    HICON SelectedIcon(IconMode mode) const {
        const HICON selected = icons[static_cast<std::size_t>(mode)];
        const HICON idle = icons[static_cast<std::size_t>(IconMode::Idle)];
        return selected ? selected : (idle ? idle : fallbackIcon);
    }

    void LoadIcons() {
        const int width = GetSystemMetrics(SM_CXSMICON);
        const int height = GetSystemMetrics(SM_CYSMICON);
        for (std::size_t index = 0; index < icons.size(); ++index) {
            icons[index] = static_cast<HICON>(LoadImageW(
                GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kIconResources[index]),
                IMAGE_ICON, width, height, LR_DEFAULTCOLOR));
        }
        fallbackIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    }

    void DestroyIcons() {
        for (HICON& icon : icons) {
            if (icon) DestroyIcon(icon);
            icon = nullptr;
        }
    }

    void SetTip(IconMode mode) {
        wcsncpy_s(iconData.szTip, StatusTip(mode), _TRUNCATE);
    }

    void ApplyRequestedMode() {
        const IconMode mode = requestedMode.load();
        if (mode == currentMode && iconData.hIcon != nullptr) return;
        currentMode = mode;
        iconData.hIcon = SelectedIcon(mode);
        SetTip(mode);
        Shell_NotifyIconW(NIM_MODIFY, &iconData);
    }

    bool Start(std::function<void()> exitCallback) {
        exitRequested = std::move(exitCallback);
        terminal = ResolveTerminalWindow();
        std::promise<bool> ready;
        std::future<bool> readyResult = ready.get_future();
        thread = std::thread([this, &ready] { ThreadMain(ready); });
        if (readyResult.get()) return true;
        thread.join();
        return false;
    }

    void ThreadMain(std::promise<bool>& ready) {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = &WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = kWindowClass;
        RegisterClassW(&windowClass);
        LoadIcons();

        // A hidden top-level window rather than a message-only one: only
        // top-level windows receive the TaskbarCreated broadcast that tells us
        // to re-add the icon after Explorer restarts.
        HWND createdWindow = CreateWindowW(
            kWindowClass, L"EasyTunnel TUI", WS_OVERLAPPED, 0, 0, 0, 0,
            nullptr, nullptr, windowClass.hInstance, this);
        window.store(createdWindow);
        if (createdWindow == nullptr || !AddIcon()) {
            Log(LogLevel::Error, "Failed to create the Windows tray icon");
            if (createdWindow != nullptr) DestroyWindow(createdWindow);
            window.store(nullptr);
            DestroyIcons();
            ready.set_value(false);
            return;
        }
        taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
        ready.set_value(true);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Shell_NotifyIconW(NIM_DELETE, &iconData);
        DestroyIcons();
        window.store(nullptr);
    }

    bool AddIcon() {
        currentMode = requestedMode.load();
        iconData = {};
        iconData.cbSize = sizeof(iconData);
        iconData.hWnd = window.load();
        iconData.uID = kTrayIconId;
        iconData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        iconData.uCallbackMessage = kTrayCallbackMessage;
        iconData.hIcon = SelectedIcon(currentMode);
        SetTip(currentMode);
        if (!Shell_NotifyIconW(NIM_ADD, &iconData)) return false;
        iconData.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &iconData);
        return true;
    }

    void UpdateStatus(bool unavailable, bool rxActive, bool txActive) {
        const IconMode mode = StatusMode(unavailable, rxActive, txActive);
        if (mode == requestedMode.exchange(mode)) return;
        const HWND iconWindow = window.load();
        if (iconWindow != nullptr) PostMessageW(iconWindow, kUpdateIconMessage, 0, 0);
    }

    void Stop() {
        if (thread.joinable()) {
            const HWND iconWindow = window.load();
            if (iconWindow != nullptr) PostMessageW(iconWindow, WM_CLOSE, 0, 0);
            thread.join();
        }
    }

    bool TerminalVisible() const {
        return terminal != nullptr && IsWindowVisible(terminal) && !IsIconic(terminal);
    }

    void ToggleTerminal() const {
        if (terminal == nullptr) return;
        if (TerminalVisible()) {
            ShowWindow(terminal, SW_HIDE);
        } else {
            ShowWindow(terminal, IsIconic(terminal) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(terminal);
        }
    }

    void ShowContextMenu() const {
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr) return;
        if (terminal != nullptr) {
            AppendMenuW(menu, MF_STRING, kToggleCommand,
                        TerminalVisible() ? L"Hide window" : L"Show window");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }
        AppendMenuW(menu, MF_STRING, kExitCommand, L"Exit");

        POINT cursor{};
        GetCursorPos(&cursor);
        const HWND iconWindow = window.load();
        SetForegroundWindow(iconWindow);
        const UINT command = TrackPopupMenu(
            menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            cursor.x, cursor.y, 0, iconWindow, nullptr);
        DestroyMenu(menu);

        if (command == kToggleCommand) ToggleTerminal();
        else if (command == kExitCommand && exitRequested) exitRequested();
        PostMessageW(iconWindow, WM_NULL, 0, 0);
    }

    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (taskbarCreatedMessage != 0 && message == taskbarCreatedMessage) {
            AddIcon();
            return 0;
        }
        if (message == kUpdateIconMessage) {
            ApplyRequestedMode();
            return 0;
        }
        if (message == kTrayCallbackMessage) {
            const UINT event = LOWORD(lParam);
            if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
                ShowContextMenu();
                return 0;
            }
            if (event == NIN_SELECT || event == NIN_KEYSELECT
                || event == WM_LBUTTONDBLCLK) {
                ToggleTerminal();
                return 0;
            }
        }
        if (message == WM_CLOSE) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                       LPARAM lParam) {
        if (message == WM_CREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return 0;
        }
        auto* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (impl != nullptr) return impl->HandleMessage(hwnd, message, wParam, lParam);
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
};

TuiWindowsTray::TuiWindowsTray() : impl_(std::make_unique<Impl>()) {}

TuiWindowsTray::~TuiWindowsTray() {
    Shutdown();
}

bool TuiWindowsTray::Init(std::function<void()> exitRequested) {
    return impl_->Start(std::move(exitRequested));
}

void TuiWindowsTray::UpdateStatus(bool unavailable, bool rxActive, bool txActive) {
    impl_->UpdateStatus(unavailable, rxActive, txActive);
}

void TuiWindowsTray::Shutdown() {
    if (impl_) impl_->Stop();
}

#endif  // _WIN32
