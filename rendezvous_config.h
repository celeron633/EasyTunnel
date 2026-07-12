#pragma once

#include <cstdint>
#include <string>

#include "log.h"

struct RendezvousConfig {
    std::string bindAddress = "0.0.0.0";
    uint16_t port = 3478;
    std::string authToken;
    uint16_t clientTimeoutSeconds = 60;
    uint16_t maxClientsPerRoom = 32;
    LogLevel logLevel = LogLevel::Info;
    std::string logFile = "EasyTunnel_rendezvous.log";
};

// Creates a default JSON file when it does not exist, then returns defaults.
bool LoadOrCreateRendezvousConfig(const std::string& path, RendezvousConfig* config,
                                  bool* created, std::string* error);
