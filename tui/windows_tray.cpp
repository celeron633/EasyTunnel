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

#include <cwchar>
#include <future>
#include <thread>
#include <utility>

#include "../log.h"

namespace {
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kToggleCommand = 1001;
constexpr UINT kExitCommand = 1002;
constexpr wchar_t kTip[] = L"EasyTunnel TUI";
constexpr wchar_t kWindowClass[] = L"EasyTunnelTuiTray";

// Under classic conhost the console window is ours to show and hide. Under
// Windows Terminal GetConsoleWindow() returns the hidden ConPTY pseudo window,
// so fall back to the window that had focus while we started up — the terminal
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
    HWND window = nullptr;
    NOTIFYICONDATAW iconData{};
    HICON icon = nullptr;
    bool ownsIcon = false;
    UINT taskbarCreatedMessage = 0;
    std::thread thread;

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

        // A hidden top-level window rather than a message-only one: only
        // top-level windows receive the TaskbarCreated broadcast that tells us
        // to re-add the icon after Explorer restarts.
        window = CreateWindowW(kWindowClass, kTip, WS_OVERLAPPED, 0, 0, 0, 0,
                               nullptr, nullptr, windowClass.hInstance, this);
        if (window == nullptr || !AddIcon()) {
            Log(LogLevel::Error, "Failed to create the Windows tray icon");
            if (window != nullptr) DestroyWindow(window);
            window = nullptr;
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
        if (icon != nullptr && ownsIcon) DestroyIcon(icon);
        icon = nullptr;
        window = nullptr;
    }

    bool AddIcon() {
        if (icon == nullptr) {
            icon = static_cast<HICON>(LoadImageW(
                GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                LR_DEFAULTCOLOR));
            ownsIcon = icon != nullptr;
            if (icon == nullptr) icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
        }
        iconData = {};
        iconData.cbSize = sizeof(iconData);
        iconData.hWnd = window;
        iconData.uID = kTrayIconId;
        iconData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        iconData.uCallbackMessage = kTrayCallbackMessage;
        iconData.hIcon = icon;
        static_assert(sizeof(kTip) <= sizeof(iconData.szTip));
        std::wmemcpy(iconData.szTip, kTip, sizeof(kTip) / sizeof(kTip[0]));
        if (!Shell_NotifyIconW(NIM_ADD, &iconData)) return false;
        iconData.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &iconData);
        return true;
    }

    void Stop() {
        if (thread.joinable()) {
            if (window != nullptr) PostMessageW(window, WM_CLOSE, 0, 0);
            thread.join();
        }
    }

    bool TerminalVisible() const {
        return terminal != nullptr && IsWindowVisible(terminal)
            && !IsIconic(terminal);
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
        SetForegroundWindow(window);
        const UINT command = TrackPopupMenu(
            menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            cursor.x, cursor.y, 0, window, nullptr);
        DestroyMenu(menu);

        if (command == kToggleCommand) ToggleTerminal();
        else if (command == kExitCommand && exitRequested) exitRequested();
        PostMessageW(window, WM_NULL, 0, 0);
    }

    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (taskbarCreatedMessage != 0 && message == taskbarCreatedMessage) {
            AddIcon();
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

void TuiWindowsTray::Shutdown() {
    if (impl_) impl_->Stop();
}

#endif  // _WIN32
