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
    NatWait,
    NatPeerInfo,
    NatArmedAck,
    NatStart,
    Error,
};

enum class NatPunchRole {
    Unknown,
    Initiator,
    Responder,
};

struct RendezvousNatPeerInfo {
    std::string mappingBehavior;
    UdpEndpoint mappedA{};
    UdpEndpoint mappedB{};
    std::string localCandidates;
};

struct NatPunchSession {
    std::string sessionId;
    uint64_t attemptId = 0;
    NatPunchRole role = NatPunchRole::Unknown;
    uint16_t protocolVersion = 0;
    std::string punchToken;
};

struct RendezvousEvent {
    RendezvousEventType type = RendezvousEventType::None;
    UdpEndpoint peer{};
    std::string peerId;
    std::vector<TraversalMode> peerCapabilities;
    std::vector<TraversalMode> traversalModes;
    std::string sessionId;
    uint64_t attemptId = 0;
    NatPunchRole natPunchRole = NatPunchRole::Unknown;
    uint16_t natPunchVersion = 0;
    std::string natPunchToken;
    RendezvousNatPeerInfo natPeerInfo;
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
    bool SendNatInfo(socket_t sock, const std::string& expectedPeerId,
                     const std::string& sessionId, uint64_t attemptId,
                     const std::string& mappingBehavior,
                     const UdpEndpoint& mappedA,
                     const UdpEndpoint& mappedB,
                     const std::string& localCandidates) const;
    bool SendNatArmed(socket_t sock, const std::string& expectedPeerId,
                      const std::string& sessionId,
                      uint64_t attemptId) const;
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
