// 6Tunnel GUI entry point

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

int main() {
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
