#include "windows_tray.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <array>
#include <cstddef>

#include "../log.h"
#include "../res/resource.h"

namespace {
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kExitCommand = 1001;

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
}  // namespace

struct WindowsTray::Impl {
    struct IconHandles {
        HICON tray = nullptr;
        HICON small = nullptr;
        HICON large = nullptr;
    };

    static Impl* active;

    GLFWwindow* glfwWindow = nullptr;
    HWND window = nullptr;
    WNDPROC previousWindowProc = nullptr;
    NOTIFYICONDATAW iconData{};
    std::array<IconHandles, static_cast<std::size_t>(IconMode::Count)> icons{};
    HICON fallbackIcon = nullptr;
    IconMode currentMode = IconMode::Disconnected;
    UINT taskbarCreatedMessage = 0;
    const bool* closeToMinimize = nullptr;
    bool exitRequested = false;
    bool iconAdded = false;

    static HICON LoadResourceIcon(int resource, int width, int height) {
        return static_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resource), IMAGE_ICON,
            width, height, LR_DEFAULTCOLOR));
    }

    void LoadIcons() {
        const int trayWidth = GetSystemMetrics(SM_CXSMICON);
        const int trayHeight = GetSystemMetrics(SM_CYSMICON);
        const int largeWidth = GetSystemMetrics(SM_CXICON);
        const int largeHeight = GetSystemMetrics(SM_CYICON);
        for (std::size_t index = 0; index < icons.size(); ++index) {
            icons[index].tray = LoadResourceIcon(kIconResources[index], trayWidth, trayHeight);
            icons[index].small = LoadResourceIcon(kIconResources[index], trayWidth, trayHeight);
            icons[index].large = LoadResourceIcon(kIconResources[index], largeWidth, largeHeight);
        }
        fallbackIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    }

    HICON TrayIcon(IconMode mode) const {
        const HICON selected = icons[static_cast<std::size_t>(mode)].tray;
        const HICON idle = icons[static_cast<std::size_t>(IconMode::Idle)].tray;
        return selected ? selected : (idle ? idle : fallbackIcon);
    }

    HICON SmallIcon(IconMode mode) const {
        const HICON selected = icons[static_cast<std::size_t>(mode)].small;
        const HICON idle = icons[static_cast<std::size_t>(IconMode::Idle)].small;
        return selected ? selected : (idle ? idle : fallbackIcon);
    }

    HICON LargeIcon(IconMode mode) const {
        const HICON selected = icons[static_cast<std::size_t>(mode)].large;
        const HICON idle = icons[static_cast<std::size_t>(IconMode::Idle)].large;
        return selected ? selected : (idle ? idle : fallbackIcon);
    }

    void SetTip(IconMode mode) {
        wcsncpy_s(iconData.szTip, StatusTip(mode), _TRUNCATE);
    }

    void ApplyMode(IconMode mode, bool modifyTray) {
        currentMode = mode;
        iconData.hIcon = TrayIcon(mode);
        SetTip(mode);
        if (modifyTray && iconAdded) Shell_NotifyIconW(NIM_MODIFY, &iconData);
        if (window) {
            SendMessageW(window, WM_SETICON, ICON_SMALL,
                         reinterpret_cast<LPARAM>(SmallIcon(mode)));
            SendMessageW(window, WM_SETICON, ICON_BIG,
                         reinterpret_cast<LPARAM>(LargeIcon(mode)));
        }
    }

    bool Init(GLFWwindow* glfwWindowValue, const bool* closeToMinimizeValue) {
        if (!glfwWindowValue || active) return false;

        glfwWindow = glfwWindowValue;
        closeToMinimize = closeToMinimizeValue;
        window = glfwGetWin32Window(glfwWindow);
        if (!window) return false;

        SetLastError(0);
        previousWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc)));
        if (!previousWindowProc && GetLastError() != 0) {
            Log(LogLevel::Error, "Failed to install the Windows tray window procedure");
            return false;
        }

        active = this;
        taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
        LoadIcons();

        iconData = {};
        iconData.cbSize = sizeof(iconData);
        iconData.hWnd = window;
        iconData.uID = kTrayIconId;
        iconData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        iconData.uCallbackMessage = kTrayCallbackMessage;
        ApplyMode(IconMode::Disconnected, false);

        if (!AddIcon()) {
            Log(LogLevel::Error, "Failed to add the Windows tray icon");
            Shutdown();
            return false;
        }
        return true;
    }

    bool AddIcon() {
        if (!Shell_NotifyIconW(NIM_ADD, &iconData)) return false;
        iconAdded = true;
        iconData.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &iconData);
        return true;
    }

    void UpdateStatus(bool unavailable, bool rxActive, bool txActive) {
        const IconMode mode = StatusMode(unavailable, rxActive, txActive);
        if (mode != currentMode) ApplyMode(mode, true);
    }

    void Shutdown() {
        if (window && iconAdded) Shell_NotifyIconW(NIM_DELETE, &iconData);
        iconAdded = false;
        if (window) {
            SendMessageW(window, WM_SETICON, ICON_SMALL, 0);
            SendMessageW(window, WM_SETICON, ICON_BIG, 0);
        }
        if (window && previousWindowProc) {
            SetWindowLongPtrW(window, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(previousWindowProc));
        }
        for (IconHandles& stateIcons : icons) {
            if (stateIcons.tray) DestroyIcon(stateIcons.tray);
            if (stateIcons.small) DestroyIcon(stateIcons.small);
            if (stateIcons.large) DestroyIcon(stateIcons.large);
            stateIcons = {};
        }
        if (active == this) active = nullptr;

        previousWindowProc = nullptr;
        window = nullptr;
        glfwWindow = nullptr;
        closeToMinimize = nullptr;
        exitRequested = false;
    }

    void HideWindow() const {
        ShowWindow(window, SW_HIDE);
    }

    void RestoreWindow() const {
        ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
    }

    void RequestExit() {
        RestoreWindow();
        exitRequested = true;
    }

    void ShowContextMenu() {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;

        AppendMenuW(menu, MF_STRING, kExitCommand, L"Exit");

        POINT cursor{};
        GetCursorPos(&cursor);
        SetForegroundWindow(window);
        const UINT command = TrackPopupMenu(
            menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            cursor.x, cursor.y, 0, window, nullptr);
        DestroyMenu(menu);

        if (command == kExitCommand) RequestExit();
        PostMessageW(window, WM_NULL, 0, 0);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (taskbarCreatedMessage != 0 && message == taskbarCreatedMessage) {
            AddIcon();
            return 0;
        }

        if (message == WM_CLOSE) {
            if (closeToMinimize && *closeToMinimize) HideWindow();
            else RequestExit();
            return 0;
        }

        if (message == WM_SYSCOMMAND && (wParam & 0xfff0U) == SC_MINIMIZE) {
            HideWindow();
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
                RestoreWindow();
                return 0;
            }
        }

        return CallWindowProcW(previousWindowProc, window, message, wParam, lParam);
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                       LPARAM lParam) {
        if (active && active->window == hwnd) {
            return active->HandleMessage(message, wParam, lParam);
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
};

WindowsTray::Impl* WindowsTray::Impl::active = nullptr;

WindowsTray::WindowsTray() : impl_(std::make_unique<Impl>()) {}

WindowsTray::~WindowsTray() {
    Shutdown();
}

bool WindowsTray::Init(GLFWwindow* window, const bool* closeToMinimize) {
    return impl_->Init(window, closeToMinimize);
}

void WindowsTray::UpdateStatus(bool unavailable, bool rxActive, bool txActive) {
    impl_->UpdateStatus(unavailable, rxActive, txActive);
}

bool WindowsTray::ConsumeExitRequest() {
    const bool requested = impl_->exitRequested;
    impl_->exitRequested = false;
    return requested;
}

void WindowsTray::Shutdown() {
    if (impl_) impl_->Shutdown();
}
