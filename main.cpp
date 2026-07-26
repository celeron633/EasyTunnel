#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "client_config.h"
#include "log.h"
#include "tunnel_engine.h"
#include "util.h"

int main(int argc, char** argv) {
    if (argc > 3) {
        std::cerr << "Usage: EasyTunnel [config.json] [target_peer_id]\n";
        return 2;
    }
    const std::string configPath = argc >= 2 ? argv[1] : kClientConfigFileName;
    const std::string targetPeerId = argc >= 3 ? argv[2] : "";

    ClientConfig config;
    bool existed = false;
    std::string error;
    if (!LoadClientConfig(configPath, &config, &existed, &error)) {
        Log(LogLevel::Error, error);
        return 1;
    }
    if (!existed) {
        // Create the shared client configuration so the user has a file to
        // edit; the GUI and TUI clients pick up the same file afterwards.
        if (!SaveClientConfig(configPath, config, &error)) {
            Log(LogLevel::Error, error);
            return 1;
        }
        Log(LogLevel::Info, "Created default configuration: " + configPath);
    }
    if (!ValidateClientConfig(config, &error)) {
        Log(LogLevel::Error, error + " (" + configPath + ")");
        return 1;
    }
    if (!targetPeerId.empty() && targetPeerId == config.peerId) {
        Log(LogLevel::Error, "Cannot connect to this client itself");
        return 1;
    }

    // An empty target registers with the rendezvous server and waits for
    // another client to select this node.
    const Config cfg = ToEngineConfig(config, targetPeerId);
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
