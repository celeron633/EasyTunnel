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
#ifdef _WIN32
// Grow the console window (and its buffer) so the whole TUI is visible without
// scrolling. Only ever enlarges; a console that is already big enough is left
// untouched, and the request is clamped to the largest size the font/monitor
// allows.
void EnlargeConsole(SHORT columns, SHORT rows) {
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return;

    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(handle, &info)) return;

    const COORD largest = GetLargestConsoleWindowSize(handle);
    if (largest.X > 0 && columns > largest.X) columns = largest.X;
    if (largest.Y > 0 && rows > largest.Y) rows = largest.Y;

    const SHORT currentCols = info.srWindow.Right - info.srWindow.Left + 1;
    const SHORT currentRows = info.srWindow.Bottom - info.srWindow.Top + 1;
    if (columns < currentCols) columns = currentCols;
    if (rows < currentRows) rows = currentRows;
    if (columns == currentCols && rows == currentRows) return;

    // The window can never be larger than the buffer, so grow the buffer first.
    COORD buffer = info.dwSize;
    if (buffer.X < columns) buffer.X = columns;
    if (buffer.Y < rows) buffer.Y = rows;
    SetConsoleScreenBufferSize(handle, buffer);

    SMALL_RECT window{0, 0, static_cast<SHORT>(columns - 1),
                      static_cast<SHORT>(rows - 1)};
    SetConsoleWindowInfo(handle, TRUE, &window);
}
#endif

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
    EnlargeConsole(110, 40);
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
