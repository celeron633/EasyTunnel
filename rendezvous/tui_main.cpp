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

#include <iostream>
#include <string>

#include "../log.h"
#include "tui_app.h"

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "Usage: EasyTunnel_rendezvous_tui [config.json]\n";
        return 2;
    }
    SetConsoleLoggingEnabled(false);
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
#endif

    RendezvousTuiApp app(argc > 1 ? argv[1] : "EasyTunnel_rendezvous.json");
    std::string error;
    if (!app.Init(&error)) {
        std::cerr << error << '\n';
#ifdef _WIN32
        WSACleanup();
#endif
        return 2;
    }
    const int result = app.Run();
#ifdef _WIN32
    WSACleanup();
#endif
    return result;
}
