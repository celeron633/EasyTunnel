#include <atomic>
#include <csignal>
#include <iostream>
#include <string>

#include "../log.h"
#include "../util.h"
#include "config.h"
#include "server.h"

namespace {
std::atomic<bool> running{true};
void Stop(int) { running.store(false); }
}  // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "Usage: EasyTunnel_rendezvous [config.json]\n";
        return 2;
    }
    const std::string configPath = argc > 1 ? argv[1] : "EasyTunnel_rendezvous.json";
    RendezvousConfig config;
    bool configCreated = false;
    std::string configError;
    if (!LoadOrCreateRendezvousConfig(configPath, &config, &configCreated, &configError)) {
        std::cerr << configError << '\n';
        return 2;
    }
    SetLogFilePath(config.logFile);
    SetLogLevel(config.logLevel);
    if (configCreated) Log(LogLevel::Info, "Created default configuration: " + configPath);
    Log(LogLevel::Info, "Loaded configuration: " + configPath
        + ", log_level=" + LevelToString(config.logLevel));

#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        Log(LogLevel::Error, "WSAStartup failed");
        return 1;
    }
#endif
    std::signal(SIGINT, Stop);
    std::signal(SIGTERM, Stop);
    RendezvousServer server(config, running);
    const int result = server.Run();
#ifdef _WIN32
    WSACleanup();
#endif
    return result;
}
