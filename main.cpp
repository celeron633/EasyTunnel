#include <chrono>
#include <string>
#include <thread>

#include "config.h"
#include "log.h"
#include "tunnel_engine.h"
#include "util.h"

int main(int argc, char** argv) {
    const std::string configPath = argc >= 2 ? argv[1] : "tunnel.conf";
    Config cfg;
    if (!LoadConfig(configPath, &cfg)) return 1;
    SetLogLevel(cfg.log_level);
    RegisterSignalHandlers();
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        Log(LogLevel::Error, "WSAStartup failed");
        return 1;
    }
#endif
    TunnelEngine engine;
    engine.SetStateCallback([](TunnelState state, const std::string& message) {
        if (state == TunnelState::Error) Log(LogLevel::Error, message);
        else if (!message.empty()) Log(LogLevel::Info, message);
    });
    engine.Start(cfg);
    while (g_running.load() && engine.GetState() != TunnelState::Error
           && engine.GetState() != TunnelState::Disconnected) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    engine.Stop();
#ifdef _WIN32
    WSACleanup();
#endif
    g_shutdownCompleted.store(true);
    return engine.GetState() == TunnelState::Error ? 1 : 0;
}
