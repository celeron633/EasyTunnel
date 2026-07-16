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

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

#include "../log.h"

namespace {
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kShowWindowCommand = 1001;
constexpr UINT kExitCommand = 1002;
constexpr UINT kDisconnectCommand = 1003;
constexpr wchar_t kWindowTitle[] = L"EasyTunnel";

struct TrayMenuItem {
    UINT command;
    const wchar_t* text;
};

constexpr TrayMenuItem kShowWindowMenuItem{
    kShowWindowCommand, L"Show Main Window"};
constexpr TrayMenuItem kDisconnectMenuItem{
    kDisconnectCommand, L"Disconnect"};
constexpr TrayMenuItem kExitMenuItem{kExitCommand, L"Exit"};

const TrayMenuItem* MenuItemFromData(ULONG_PTR itemData) {
    const auto* item = reinterpret_cast<const TrayMenuItem*>(itemData);
    if (item == &kShowWindowMenuItem || item == &kDisconnectMenuItem
        || item == &kExitMenuItem) {
        return item;
    }
    return nullptr;
}

std::wstring FormatTraffic(uint64_t bytes) {
    constexpr const wchar_t* kUnits[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    constexpr std::size_t kUnitCount = sizeof(kUnits) / sizeof(kUnits[0]);
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < kUnitCount) {
        value /= 1024.0;
        ++unit;
    }

    std::wostringstream output;
    if (unit == 0) {
        output << bytes;
    } else {
        output << std::fixed << std::setprecision(value < 10.0 ? 2 : 1) << value;
    }
    output << L' ' << kUnits[unit];
    return output.str();
}
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
    std::wstring tooltipStatus;
    std::wstring tooltipText = kWindowTitle;
    std::chrono::steady_clock::time_point lastTooltipUpdate{};
    std::function<void()> disconnectCallback;
    std::function<void()> exitCallback;

    bool Init(GLFWwindow* glfwWindowValue,
              std::function<void()> disconnectCallbackValue,
              std::function<void()> exitCallbackValue) {
        if (!glfwWindowValue || active) return false;

        glfwWindow = glfwWindowValue;
        disconnectCallback = std::move(disconnectCallbackValue);
        exitCallback = std::move(exitCallbackValue);
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
        CopyTooltipText(tooltipText);

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

    void CopyTooltipText(const std::wstring& text) {
        constexpr std::size_t capacity = sizeof(iconData.szTip) / sizeof(iconData.szTip[0]);
        std::wcsncpy(iconData.szTip, text.c_str(), capacity - 1);
        iconData.szTip[capacity - 1] = L'\0';
    }

    void UpdateTooltip(const wchar_t* status,
                       uint64_t sentBytes,
                       uint64_t receivedBytes,
                       int64_t latencyMilliseconds) {
        if (!window) return;

        const std::wstring nextStatus = status ? status : L"Unknown";
        const auto now = std::chrono::steady_clock::now();
        const bool statusChanged = nextStatus != tooltipStatus;
        if (!statusChanged && now - lastTooltipUpdate < std::chrono::seconds(1)) return;
        lastTooltipUpdate = now;

        std::wostringstream tooltip;
        tooltip << kWindowTitle << L"\nStatus: " << nextStatus
                << L"\nSent: " << FormatTraffic(sentBytes)
                << L" | Received: " << FormatTraffic(receivedBytes)
                << L"\nLatency: ";
        if (latencyMilliseconds < 0) tooltip << L"--";
        else tooltip << latencyMilliseconds;
        tooltip << L" ms";

        const std::wstring nextText = tooltip.str();
        tooltipStatus = nextStatus;
        if (nextText == tooltipText) return;

        tooltipText = nextText;
        CopyTooltipText(tooltipText);
        Shell_NotifyIconW(NIM_MODIFY, &iconData);
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
        tooltipStatus.clear();
        tooltipText = kWindowTitle;
        lastTooltipUpdate = {};
        disconnectCallback = {};
        exitCallback = {};
    }

    void HideWindow() const {
        ShowWindow(window, SW_HIDE);
    }

    void RestoreWindow() const {
        ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
    }

    void ShowContextMenu() const {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;

        AppendMenuW(menu, MF_OWNERDRAW, kShowWindowMenuItem.command,
                    reinterpret_cast<LPCWSTR>(&kShowWindowMenuItem));
        AppendMenuW(menu, MF_OWNERDRAW, kDisconnectMenuItem.command,
                    reinterpret_cast<LPCWSTR>(&kDisconnectMenuItem));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_OWNERDRAW, kExitMenuItem.command,
                    reinterpret_cast<LPCWSTR>(&kExitMenuItem));

        POINT cursor{};
        GetCursorPos(&cursor);
        SetForegroundWindow(window);
        const UINT command = TrackPopupMenu(
            menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            cursor.x, cursor.y, 0, window, nullptr);
        DestroyMenu(menu);

        if (command == kShowWindowCommand) RestoreWindow();
        if (command == kDisconnectCommand && disconnectCallback) {
            RestoreWindow();
            disconnectCallback();
        }
        if (command == kExitCommand && exitCallback) {
            RestoreWindow();
            exitCallback();
        }
        PostMessageW(window, WM_NULL, 0, 0);
    }

    bool MeasureMenuItem(MEASUREITEMSTRUCT* measure) const {
        if (!measure || measure->CtlType != ODT_MENU) return false;
        const TrayMenuItem* item = MenuItemFromData(measure->itemData);
        if (!item) return false;

        HDC dc = GetDC(window);
        if (!dc) return false;
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ previousFont = SelectObject(dc, font);
        SIZE textSize{};
        GetTextExtentPoint32W(dc, item->text, static_cast<int>(std::wcslen(item->text)),
                              &textSize);
        SelectObject(dc, previousFont);
        ReleaseDC(window, dc);

        const UINT horizontalPadding = static_cast<UINT>(GetSystemMetrics(SM_CXMENUCHECK));
        const UINT verticalPadding = 8;
        measure->itemWidth = static_cast<UINT>(textSize.cx) + horizontalPadding * 2;
        measure->itemHeight = std::max(static_cast<UINT>(GetSystemMetrics(SM_CYMENU)),
                                       static_cast<UINT>(textSize.cy) + verticalPadding);
        return true;
    }

    bool DrawMenuItem(DRAWITEMSTRUCT* draw) const {
        if (!draw || draw->CtlType != ODT_MENU) return false;
        const TrayMenuItem* item = MenuItemFromData(draw->itemData);
        if (!item) return false;

        const bool selected = (draw->itemState & ODS_SELECTED) != 0;
        FillRect(draw->hDC, &draw->rcItem,
                 GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_MENU));

        const int previousBackgroundMode = SetBkMode(draw->hDC, TRANSPARENT);
        const COLORREF previousTextColor = SetTextColor(
            draw->hDC, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT));
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ previousFont = SelectObject(draw->hDC, font);

        RECT textRect = draw->rcItem;
        textRect.left += GetSystemMetrics(SM_CXMENUCHECK);
        DrawTextW(draw->hDC, item->text, -1, &textRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        SelectObject(draw->hDC, previousFont);
        SetTextColor(draw->hDC, previousTextColor);
        SetBkMode(draw->hDC, previousBackgroundMode);
        return true;
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (taskbarCreatedMessage != 0 && message == taskbarCreatedMessage) {
            AddIcon();
            return 0;
        }

        if (message == WM_CLOSE) {
            HideWindow();
            return 0;
        }

        if (message == WM_SYSCOMMAND && (wParam & 0xfff0U) == SC_MINIMIZE) {
            HideWindow();
            return 0;
        }

        if (message == WM_MEASUREITEM
            && MeasureMenuItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lParam))) {
            return TRUE;
        }

        if (message == WM_DRAWITEM
            && DrawMenuItem(reinterpret_cast<DRAWITEMSTRUCT*>(lParam))) {
            return TRUE;
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

bool WindowsTray::Init(GLFWwindow* window,
                       std::function<void()> disconnectCallback,
                       std::function<void()> exitCallback) {
    return impl_->Init(window, std::move(disconnectCallback), std::move(exitCallback));
}

void WindowsTray::UpdateTooltip(const wchar_t* status,
                                uint64_t sentBytes,
                                uint64_t receivedBytes,
                                int64_t latencyMilliseconds) {
    if (impl_) {
        impl_->UpdateTooltip(status, sentBytes, receivedBytes, latencyMilliseconds);
    }
}

void WindowsTray::Shutdown() {
    if (impl_) impl_->Shutdown();
}
