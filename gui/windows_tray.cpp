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

#include <cwchar>

#include "../log.h"

namespace {
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kExitCommand = 1001;
constexpr wchar_t kWindowTitle[] = L"EasyTunnel";
}  // namespace

struct WindowsTray::Impl {
    static Impl* active;

    GLFWwindow* glfwWindow = nullptr;
    HWND window = nullptr;
    WNDPROC previousWindowProc = nullptr;
    NOTIFYICONDATAW iconData{};
    HICON icon = nullptr;
    bool ownsIcon = false;
    UINT taskbarCreatedMessage = 0;

    bool Init(GLFWwindow* glfwWindowValue) {
        if (!glfwWindowValue || active) return false;

        glfwWindow = glfwWindowValue;
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

        icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
                                             IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                             GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        ownsIcon = icon != nullptr;
        if (!icon) icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));

        iconData = {};
        iconData.cbSize = sizeof(iconData);
        iconData.hWnd = window;
        iconData.uID = kTrayIconId;
        iconData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        iconData.uCallbackMessage = kTrayCallbackMessage;
        iconData.hIcon = icon;
        static_assert(sizeof(kWindowTitle) <= sizeof(iconData.szTip));
        std::wmemcpy(iconData.szTip, kWindowTitle,
                     sizeof(kWindowTitle) / sizeof(kWindowTitle[0]));

        if (!AddIcon()) {
            Log(LogLevel::Error, "Failed to add the Windows tray icon");
            Shutdown();
            return false;
        }
        return true;
    }

    bool AddIcon() {
        if (!Shell_NotifyIconW(NIM_ADD, &iconData)) return false;
        iconData.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &iconData);
        return true;
    }

    void Shutdown() {
        if (window) Shell_NotifyIconW(NIM_DELETE, &iconData);
        if (window && previousWindowProc) {
            SetWindowLongPtrW(window, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(previousWindowProc));
        }
        if (icon && ownsIcon) DestroyIcon(icon);
        if (active == this) active = nullptr;

        icon = nullptr;
        ownsIcon = false;
        previousWindowProc = nullptr;
        window = nullptr;
        glfwWindow = nullptr;
    }

    void HideWindow() const {
        ShowWindow(window, SW_HIDE);
    }

    void RestoreWindow() const {
        ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
    }

    void ConfirmExit() const {
        const int result = MessageBoxW(
            window, L"Exit EasyTunnel?", kWindowTitle,
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND);
        if (result == IDYES) {
            glfwSetWindowShouldClose(glfwWindow, GLFW_TRUE);
        } else {
            HideWindow();
        }
    }

    void ShowContextMenu() const {
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

        if (command == kExitCommand) ConfirmExit();
        PostMessageW(window, WM_NULL, 0, 0);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (taskbarCreatedMessage != 0 && message == taskbarCreatedMessage) {
            AddIcon();
            return 0;
        }

        if (message == WM_CLOSE) {
            ConfirmExit();
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
            if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONDBLCLK) {
                RestoreWindow();
                return 0;
            }
        }

        return CallWindowProcW(previousWindowProc, window, message, wParam, lParam);
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
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

bool WindowsTray::Init(GLFWwindow* window) {
    return impl_->Init(window);
}

void WindowsTray::Shutdown() {
    if (impl_) impl_->Shutdown();
}
