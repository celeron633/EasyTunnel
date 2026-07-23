#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../util.h"
#include "config.h"

struct RendezvousClientSnapshot {
    std::string nodeId;
    std::string endpoint;
    std::string tunIp;
    std::string pairedWith;
    uint64_t idleSeconds = 0;
    uint32_t nat4Round = 0;
    bool nat4Joined = false;
};

struct RendezvousRoomSnapshot {
    std::string roomId;
    std::vector<RendezvousClientSnapshot> clients;
};

struct RendezvousRelaySnapshot {
    uint64_t activeSessions = 0;
    uint64_t receivedDatagrams = 0;
    uint64_t forwardedDatagrams = 0;
    uint64_t forwardedBytes = 0;
    struct Peer {
        std::string nodeId;
        std::string endpoint;
        uint64_t idleSeconds = 0;
        bool connected = false;
    };
    struct Session {
        std::string roomId;
        uint16_t port = 0;
        bool ready = false;
        Peer peers[2];
    };
    std::vector<Session> sessions;
};

class RendezvousRegistry {
public:
    RendezvousRegistry(socket_t sock, const RendezvousConfig& config);
    ~RendezvousRegistry();

    RendezvousRegistry(const RendezvousRegistry&) = delete;
    RendezvousRegistry& operator=(const RendezvousRegistry&) = delete;

    void Handle(const UdpEndpoint& source, const std::string& type,
                const std::vector<std::string>& fields,
                std::chrono::steady_clock::time_point now);
    std::vector<RendezvousRoomSnapshot> Snapshot(
        std::chrono::steady_clock::time_point now);
    RendezvousRelaySnapshot RelaySnapshot();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
