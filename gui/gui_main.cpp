// EasyTunnel GUI entry point

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

#include "gui_app.h"
#include "../log.h"

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

int main() {
	SetLogFilePath(GuiLogPath());
#ifdef _WIN32
	WSADATA wsa{};
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		Log(LogLevel::Error, "WSAStartup failed");
		return 1;
	}
#endif

	GuiApp app;
	if (!app.Init()) {
		Log(LogLevel::Error, "Failed to initialize GUI");
#ifdef _WIN32
		WSACleanup();
#endif
		return 1;
	}

	app.Run();
	app.Shutdown();

#ifdef _WIN32
	WSACleanup();
#endif
	return 0;
}
