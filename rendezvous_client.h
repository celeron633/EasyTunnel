#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "config.h"
#include "util.h"

enum class RendezvousEventType {
    None,
    Registered,
    PeerUnavailable,
    Peer,
    Nat4Wait,
    Nat4Peer,
    Error,
};

struct RendezvousEvent {
    RendezvousEventType type = RendezvousEventType::None;
    UdpEndpoint peer{};
    std::string peerId;
    std::vector<TraversalMode> peerCapabilities;
    std::vector<TraversalMode> traversalModes;
    uint32_t round = 0;
    std::string error;
};

// One entry of the online client list published by the rendezvous server.
// `endpoint` is the public address the server sees, `tunIp` stays empty until
// the peer has reported its tunnel address.
struct RendezvousPeerInfo {
    std::string peerId;
    std::string endpoint;
    std::vector<TraversalMode> capabilities;
    std::string tunIp;
    uint64_t idleSeconds = 0;
};

// Handles control traffic exchanged with one configured rendezvous endpoint.
// The caller retains the UDP receive loop so peer punch packets can share the socket.
class RendezvousClient {
public:
    RendezvousClient(const Config& config, const UdpEndpoint& server);

    bool SendProbe(socket_t sock) const;
    bool SendNat4Join(socket_t sock, const std::string& expectedPeerId,
                      uint32_t round) const;
    void Unregister(socket_t sock) const;

    RendezvousEvent HandlePacket(const UdpEndpoint& source,
                                 const uint8_t* data, size_t len);
    bool HasResponded() const;
    bool ResponseTimedOut(std::chrono::steady_clock::time_point now,
                          std::string* error) const;
    bool HandleUnreachableError(int socketError, std::string* error) const;

private:
    const Config& config_;
    UdpEndpoint server_;
    std::chrono::steady_clock::time_point responseDeadline_;
    bool responded_ = false;
};

bool ValidateRendezvousSession(const Config& config, std::string* error);
bool OpenRendezvousSocket(const Config& config, int recvTimeoutMs,
                          socket_t* sock, UdpEndpoint* server,
                          std::string* error);
void UnregisterRendezvous(socket_t sock, const Config& config,
                          const UdpEndpoint& server);
bool ReportRendezvousTunIp(socket_t sock, const Config& config,
                           const UdpEndpoint& server);
bool ListRendezvousClients(const std::string& serverAddress, uint16_t serverPort,
                           const std::string& roomId, const std::string& authToken,
                           std::vector<RendezvousPeerInfo>* clients,
                           std::string* error);
std::string FormatPeerCapabilities(const std::vector<TraversalMode>& capabilities);
