// EasyTunnel GUI entry point

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#endif

#include <QApplication>

#include "../log.h"
#include "gui_theme.h"
#include "main_window.h"

namespace {
std::string GuiLogPath() {
#ifdef _WIN32
    char executable[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(nullptr, executable, MAX_PATH);
    std::string path(executable, length);
    const size_t slash = path.find_last_of("\\/");
    return (slash == std::string::npos ? std::string() : path.substr(0, slash + 1))
        + "EasyTunnel_gui.log";
#else
    return "EasyTunnel_gui.log";
#endif
}
}  // namespace

int main(int argc, char* argv[]) {
    SetLogFilePath(GuiLogPath());
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        Log(LogLevel::Error, "WSAStartup failed");
        return 1;
    }
#endif

    int result = 0;
    {
        QApplication app(argc, argv);
        QApplication::setApplicationName(QStringLiteral("EasyTunnel"));
        // The window may live in the notification area; exit is explicit.
        QApplication::setQuitOnLastWindowClosed(false);
        gui_theme::Apply(app);

        MainWindow window;
        window.show();
        result = app.exec();
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return result;
}
