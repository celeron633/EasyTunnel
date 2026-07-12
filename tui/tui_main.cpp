#ifdef _WIN32
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <winsock2.h>
#include <windows.h>
#endif

#include <string>

#include "../log.h"
#include "tui_app.h"

namespace {
std::string TuiLogPath() {
#ifdef _WIN32
    char executable[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(nullptr, executable, MAX_PATH);
    std::string path(executable, length);
    const size_t slash = path.find_last_of("\\/");
    return (slash == std::string::npos ? std::string() : path.substr(0, slash + 1))
        + "EasyTunnel_tui.log";
#else
    return "EasyTunnel_tui.log";
#endif
}
}  // namespace

int main() {
    SetConsoleLoggingEnabled(false);
    SetLogFilePath(TuiLogPath());
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        Log(LogLevel::Error, "WSAStartup failed");
        return 1;
    }
#endif
    TuiApp app;
    if (!app.Init()) {
        Log(LogLevel::Error, "Failed to initialize TUI");
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    const int result = app.Run();
#ifdef _WIN32
    WSACleanup();
#endif
    return result;
}
