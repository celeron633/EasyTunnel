#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../util.h"
#include "config.h"

struct Ipv4RelayPeerSnapshot {
    std::string nodeId;
    std::string endpoint;
    uint64_t idleSeconds = 0;
    bool connected = false;
};

struct Ipv4RelaySessionSnapshot {
    std::string roomId;
    uint16_t port = 0;
    bool ready = false;
    Ipv4RelayPeerSnapshot peers[2];
};

struct Ipv4RelayAppSnapshot {
    uint64_t activeSessions = 0;
    uint64_t receivedDatagrams = 0;
    uint64_t forwardedDatagrams = 0;
    uint64_t forwardedBytes = 0;
    std::vector<Ipv4RelaySessionSnapshot> sessions;
};

// Owns the rendezvous server's IPv4 UDP relay data plane. Each Peer pair gets
// one UDP socket, one public port and one worker thread. The rendezvous registry
// only validates control-plane membership and delegates RELAY_JOIN here.
class Ipv4RelayApp {
public:
    Ipv4RelayApp(socket_t controlSocket, const RendezvousConfig& config);
    ~Ipv4RelayApp();

    Ipv4RelayApp(const Ipv4RelayApp&) = delete;
    Ipv4RelayApp& operator=(const Ipv4RelayApp&) = delete;

    void HandleJoin(const UdpEndpoint& source, const std::string& roomId,
                    const std::string& nodeId, const std::string& targetId,
                    std::chrono::steady_clock::time_point now);
    void RemovePeer(const std::string& roomId, const std::string& nodeId);
    Ipv4RelayAppSnapshot Snapshot();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
