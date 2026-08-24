#include "rendezvous_client.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "nat_protocol.h"

namespace {
constexpr auto kRendezvousResponseTimeout = std::chrono::seconds(5);
constexpr const char* kNoResponseError = "Rendezvous server did not respond";
// Fields per client in a CLIENTS reply, mirroring the rendezvous registry.
constexpr size_t kClientFields = 5;
constexpr size_t kMaxControlMessageBytes = 8192;
constexpr const char* kUnknownField = "-";

bool Send(socket_t sock, const UdpEndpoint& endpoint, const std::string& data) {
    return sendto(sock, data.data(), static_cast<int>(data.size()), 0,
        reinterpret_cast<const sockaddr*>(&endpoint.addr), endpoint.addr_len)
        == static_cast<int>(data.size());
}

UdpEndpoint FromSockaddr(const sockaddr_storage& address, socket_len_t len) {
    UdpEndpoint endpoint{};
    endpoint.addr = address;
    endpoint.addr_len = len;
    endpoint.family = address.ss_family;
    return endpoint;
}

bool ParseAttemptId(const std::string& text, uint64_t* attemptId) {
    try {
        size_t consumed = 0;
        const unsigned long long value = std::stoull(text, &consumed);
        if (consumed != text.size() || value == 0) return false;
        *attemptId = static_cast<uint64_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParsePort(const std::string& text, uint16_t* port) {
    try {
        size_t consumed = 0;
        const unsigned long value = std::stoul(text, &consumed);
        if (consumed != text.size() || value == 0 || value > 65535) return false;
        *port = static_cast<uint16_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseRole(const std::string& text, NatPunchRole* role) {
    if (text == "initiator") {
        *role = NatPunchRole::Initiator;
        return true;
    }
    if (text == "responder") {
        *role = NatPunchRole::Responder;
        return true;
    }
    return false;
}

bool IsNatMappingBehavior(const std::string& behavior) {
    return behavior == "endpoint-independent"
        || behavior == "port-dependent-regular"
        || behavior == "port-dependent-random"
        || behavior == "multi-public-ip"
        || behavior == "unknown";
}

bool ParseMappedEndpoint(const std::string& ip, const std::string& portText,
                         UdpEndpoint* endpoint) {
    uint16_t port = 0;
    return ParsePort(portText, &port) && ParseUdpEndpoint(ip, port, endpoint);
}

bool IpAndPort(const UdpEndpoint& endpoint, std::string* ip,
               std::string* port) {
    if (endpoint.family != AF_INET
        || endpoint.addr_len < static_cast<socket_len_t>(sizeof(sockaddr_in))) {
        return false;
    }
    const auto* address = reinterpret_cast<const sockaddr_in*>(&endpoint.addr);
    char text[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) == nullptr) {
        return false;
    }
    *ip = text;
    *port = std::to_string(ntohs(address->sin_port));
    return true;
}

bool ParsePeer(const std::vector<std::string>& fields, UdpEndpoint* peer,
               std::string* peerId) {
    if (fields.size() < 3) return false;
    unsigned long port = 0;
    try {
        size_t consumed = 0;
        port = std::stoul(fields[1], &consumed);
        if (consumed != fields[1].size()) return false;
    } catch (...) {
        return false;
    }
    if (port == 0 || port > 65535
        || !ParseUdpEndpoint(fields[0], static_cast<uint16_t>(port), peer)) {
        return false;
    }
    *peerId = fields[2];
    return true;
}

bool ParseIdleSeconds(const std::string& text, uint64_t* seconds) {
    try {
        size_t consumed = 0;
        const unsigned long long value = std::stoull(text, &consumed);
        if (consumed != text.size()) return false;
        *seconds = static_cast<uint64_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

RendezvousEvent InvalidResponse() {
    RendezvousEvent event;
    event.type = RendezvousEventType::Error;
    event.error = "Invalid rendezvous response";
    return event;
}

RendezvousEvent ProtocolVersionMismatch(const std::string& received) {
    RendezvousEvent event;
    event.type = RendezvousEventType::Error;
    const std::string safeReceived = IsSafeControlField(received)
        ? received : received.empty() ? "legacy" : "invalid";
    event.error = "NAT punch protocol version mismatch: expected "
        + std::string(kNatPunchProtocolVersion) + ", received "
        + safeReceived
        + "; update EasyTunnel clients and rendezvous server together";
    return event;
}
}  // namespace

RendezvousClient::RendezvousClient(const Config& config,
                                   const UdpEndpoint& server)
    : config_(config),
      server_(server),
      responseDeadline_(std::chrono::steady_clock::now()
                        + kRendezvousResponseTimeout) {}

bool RendezvousClient::SendProbe(socket_t sock) const {
    const std::string capabilities = SerializeTraversalModeSequence(
        EnabledTraversalModes(config_.traversal_modes));
    bool sent = Send(sock, server_, MakeControlMessage("REG",
        {config_.room_id, config_.peer_id, capabilities,
         kNatPunchProtocolVersion, config_.auth_token}));
    if (!config_.target_peer_id.empty()) {
        sent = Send(sock, server_, MakeControlMessage("CONNECT",
            {config_.room_id, config_.peer_id,
             config_.target_peer_id, capabilities, kNatPunchProtocolVersion,
             config_.auth_token})) && sent;
    }
    return sent;
}

bool RendezvousClient::SendNatInfo(
        socket_t sock, const std::string& expectedPeerId,
        const std::string& sessionId, uint64_t attemptId,
        const std::string& mappingBehavior, const UdpEndpoint& mappedA,
        const UdpEndpoint& mappedB,
        const std::string& localCandidates) const {
    std::string mappedAIp;
    std::string mappedAPort;
    std::string mappedBIp;
    std::string mappedBPort;
    if (!IpAndPort(mappedA, &mappedAIp, &mappedAPort)
        || !IpAndPort(mappedB, &mappedBIp, &mappedBPort)
        || !IsNatMappingBehavior(mappingBehavior)
        || !IsSafeControlField(sessionId)
        || !IsSafeControlField(localCandidates)) {
        return false;
    }
    return Send(sock, server_, MakeControlMessage("NAT_INFO",
        {config_.room_id, config_.peer_id, expectedPeerId, sessionId,
         std::to_string(attemptId), kNatPunchProtocolVersion,
         mappingBehavior, mappedAIp, mappedAPort, mappedBIp, mappedBPort,
         localCandidates, config_.auth_token}));
}

bool RendezvousClient::SendNatArmed(
        socket_t sock, const std::string& expectedPeerId,
        const std::string& sessionId, uint64_t attemptId) const {
    return Send(sock, server_, MakeControlMessage("NAT_ARMED",
        {config_.room_id, config_.peer_id, expectedPeerId, sessionId,
         std::to_string(attemptId), config_.auth_token}));
}

bool RendezvousClient::SendNatRetry(
        socket_t sock, const std::string& expectedPeerId,
        const std::string& sessionId, uint64_t attemptId) const {
    if (!IsSafeControlField(sessionId) || attemptId == 0) return false;
    return Send(sock, server_, MakeControlMessage("NAT_RETRY",
        {config_.room_id, config_.peer_id, expectedPeerId, sessionId,
         std::to_string(attemptId), config_.auth_token}));
}

void RendezvousClient::Unregister(socket_t sock) const {
    UnregisterRendezvous(sock, config_, server_);
}

RendezvousEvent RendezvousClient::HandlePacket(const UdpEndpoint& source,
                                               const uint8_t* data, size_t len) {
    if (!SameUdpEndpoint(source, server_)) return {};

    std::string type;
    std::vector<std::string> fields;
    if (!ParseControlMessage(data, len, &type, &fields)) return InvalidResponse();
    responded_ = true;

    RendezvousEvent event;
    if (type == "REGISTERED") {
        if (fields.size() != 1) {
            return ProtocolVersionMismatch(
                fields.empty() ? "legacy" : fields[0]);
        }
        if (fields[0] != kNatPunchProtocolVersion) {
            return ProtocolVersionMismatch(fields[0]);
        }
        event.type = RendezvousEventType::Registered;
        return event;
    }
    if (type == "ERROR") {
        if (!fields.empty() && fields[0] == "peer-not-found") {
            event.type = RendezvousEventType::PeerUnavailable;
        } else if (fields.size() == 2
                   && fields[0] == "no-common-traversal-mode") {
            std::vector<TraversalMode> peerCapabilities;
            std::string parseError;
            if (!ParseTraversalModeSequence(
                    fields[1], &peerCapabilities, &parseError)) {
                return InvalidResponse();
            }
            event.type = RendezvousEventType::Error;
            event.peerCapabilities = std::move(peerCapabilities);
            event.error = "Peer " + config_.target_peer_id
                + " does not support any enabled traversal mode"
                + " (peer supports: " + fields[1] + ")";
        } else if (fields.size() == 3
                   && fields[0] == "nat-punch-version-mismatch") {
            return ProtocolVersionMismatch(fields[2]);
        } else {
            event.type = RendezvousEventType::Error;
            event.error = fields.empty()
                ? "Rendezvous rejected request"
                : "Rendezvous error: " + fields[0];
        }
        return event;
    }
    if (type == "PEER") {
        if (fields.size() != 10) {
            return ProtocolVersionMismatch("legacy");
        }
        if (fields[8] != kNatPunchProtocolVersion) {
            return ProtocolVersionMismatch(fields[8]);
        }
        std::string parseError;
        if (!ParsePeer(fields, &event.peer, &event.peerId)
            || !ParseTraversalModeSequence(
                fields[3], &event.peerCapabilities, &parseError)
            || !ParseTraversalModeSequence(
                fields[4], &event.traversalModes, &parseError)
            || !IsSafeControlField(fields[5])
            || !ParseAttemptId(fields[6], &event.attemptId)
            || !ParseRole(fields[7], &event.natPunchRole)
            || !IsSafeControlField(fields[9])
            || event.traversalModes.empty()) {
            return InvalidResponse();
        }
        const std::vector<TraversalMode> localCapabilities =
            EnabledTraversalModes(config_.traversal_modes);
        for (const TraversalMode mode : event.traversalModes) {
            if (std::find(localCapabilities.begin(), localCapabilities.end(), mode)
                    == localCapabilities.end()
                || std::find(event.peerCapabilities.begin(),
                             event.peerCapabilities.end(), mode)
                    == event.peerCapabilities.end()) {
                return InvalidResponse();
            }
        }
        event.sessionId = fields[5];
        event.natPunchVersion = kNatPunchProtocolVersionNumber;
        event.natPunchToken = fields[9];
        event.type = RendezvousEventType::Peer;
        return event;
    }
    if (type == "NAT_WAIT" || type == "NAT_ARMED_ACK"
        || type == "NAT_START" || type == "NAT_RETRY_WAIT") {
        if (fields.size() != 2 || !IsSafeControlField(fields[0])
            || !ParseAttemptId(fields[1], &event.attemptId)) {
            return InvalidResponse();
        }
        event.sessionId = fields[0];
        event.type = type == "NAT_WAIT" ? RendezvousEventType::NatWait
            : type == "NAT_ARMED_ACK" ? RendezvousEventType::NatArmedAck
            : type == "NAT_START" ? RendezvousEventType::NatStart
            : RendezvousEventType::NatRetryWait;
        return event;
    }
    if (type == "NAT_ATTEMPT") {
        if (fields.size() != 5) {
            return ProtocolVersionMismatch("legacy");
        }
        if (fields[3] != kNatPunchProtocolVersion) {
            return ProtocolVersionMismatch(fields[3]);
        }
        if (!IsSafeControlField(fields[0])
            || !ParseAttemptId(fields[1], &event.attemptId)
            || !ParseRole(fields[2], &event.natPunchRole)
            || fields[4].empty() || !IsSafeControlField(fields[4])) {
            return InvalidResponse();
        }
        event.sessionId = fields[0];
        event.natPunchVersion = kNatPunchProtocolVersionNumber;
        event.natPunchToken = fields[4];
        event.type = RendezvousEventType::NatAttempt;
        return event;
    }
    if (type == "NAT_PEER_INFO") {
        if (fields.size() != 11) {
            return ProtocolVersionMismatch("legacy");
        }
        if (fields[10] != kNatPunchProtocolVersion) {
            return ProtocolVersionMismatch(fields[10]);
        }
        if (!IsSafeControlField(fields[0])
            || !ParseAttemptId(fields[1], &event.attemptId)
            || !IsSafeControlField(fields[2])
            || !IsNatMappingBehavior(fields[3])
            || !ParseMappedEndpoint(fields[4], fields[5],
                                    &event.natPeerInfo.mappedA)
            || !ParseMappedEndpoint(fields[6], fields[7],
                                    &event.natPeerInfo.mappedB)
            || !IsSafeControlField(fields[8])
            || !ParseRole(fields[9], &event.natPunchRole)) {
            return InvalidResponse();
        }
        event.sessionId = fields[0];
        event.peerId = fields[2];
        event.natPeerInfo.mappingBehavior = fields[3];
        event.natPeerInfo.localCandidates = fields[8];
        event.natPunchVersion = kNatPunchProtocolVersionNumber;
        event.type = RendezvousEventType::NatPeerInfo;
        return event;
    }

    event.type = RendezvousEventType::Error;
    event.error = "Unexpected rendezvous response: " + type;
    return event;
}

bool RendezvousClient::HasResponded() const {
    return responded_;
}

bool RendezvousClient::ResponseTimedOut(
    std::chrono::steady_clock::time_point now, std::string* error) const {
    if (responded_ || now < responseDeadline_) return false;
    *error = kNoResponseError;
    return true;
}

bool RendezvousClient::HandleUnreachableError(int socketError,
                                              std::string* error) const {
    if (responded_ || !IsUdpDestinationUnreachable(socketError)) return false;
    *error = kNoResponseError;
    return true;
}

bool ValidateRendezvousSession(const Config& config, std::string* error) {
    in_addr tunIp{};
    if (!IsSafeControlField(config.room_id)
        || !IsSafeControlField(config.peer_id)
        || (!config.target_peer_id.empty()
            && !IsSafeControlField(config.target_peer_id))
        || (!config.auth_token.empty()
            && !IsSafeControlField(config.auth_token))
        || (!config.local_tun_ipv4.empty()
            && (!IsSafeControlField(config.local_tun_ipv4)
                || !ParseIpv4(config.local_tun_ipv4, &tunIp)))) {
        *error = "room_id/peer_id/target_peer_id/auth_token/local_tun_ipv4 is invalid";
        return false;
    }
    return true;
}

bool OpenRendezvousSocket(const Config& config, int recvTimeoutMs,
                          socket_t* sock, UdpEndpoint* server,
                          std::string* error) {
    if (!ResolveUdpEndpoint(config.rendezvous_addr, config.rendezvous_port,
                            AF_INET, server, error)) return false;
    socket_t opened = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (opened == kInvalidSocket) {
        *error = "Cannot create IPv4 UDP socket. err="
            + std::to_string(GetSocketError());
        return false;
    }
    SetSocketRecvTimeoutMs(opened, recvTimeoutMs);
    *sock = opened;
    return true;
}

void UnregisterRendezvous(socket_t sock, const Config& config,
                          const UdpEndpoint& server) {
    if (sock == kInvalidSocket) return;
    Send(sock, server, MakeControlMessage("UNREG",
        {config.room_id, config.peer_id, config.auth_token}));
}

bool ReportRendezvousTunIp(socket_t sock, const Config& config,
                           const UdpEndpoint& server) {
    if (sock == kInvalidSocket || config.local_tun_ipv4.empty()) return true;
    return Send(sock, server, MakeControlMessage("TUN_IP",
        {config.room_id, config.peer_id,
         config.local_tun_ipv4, config.auth_token}));
}

bool ListRendezvousClients(const std::string& serverAddress, uint16_t serverPort,
                           const std::string& roomId, const std::string& authToken,
                           std::vector<RendezvousPeerInfo>* clients,
                           std::string* error) {
    clients->clear();
    if (!IsSafeControlField(roomId)
        || (!authToken.empty() && !IsSafeControlField(authToken))) {
        *error = "Invalid room ID or token";
        return false;
    }
    UdpEndpoint server{};
    if (!ResolveUdpEndpoint(serverAddress, serverPort, AF_INET, &server, error)) return false;
    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSocket) {
        *error = "Cannot create UDP socket";
        return false;
    }
    SetSocketRecvTimeoutMs(sock, 1500);
    const std::string request = MakeControlMessage("LIST", {roomId, authToken});
    if (!Send(sock, server, request)) {
        *error = "Cannot send client-list request";
        CloseSocket(sock);
        return false;
    }
    std::vector<uint8_t> buffer(kMaxControlMessageBytes);
    sockaddr_storage sourceAddress{};
    socket_len_t sourceLen = static_cast<socket_len_t>(sizeof(sourceAddress));
    const int n = recvfrom(sock, reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(buffer.size()), 0,
        reinterpret_cast<sockaddr*>(&sourceAddress), &sourceLen);
    CloseSocket(sock);
    if (n < 0) {
        *error = kNoResponseError;
        return false;
    }
    const UdpEndpoint source = FromSockaddr(sourceAddress, sourceLen);
    std::string type;
    std::vector<std::string> fields;
    if (!SameUdpEndpoint(source, server)
        || !ParseControlMessage(buffer.data(), static_cast<size_t>(n), &type, &fields)) {
        *error = "Invalid rendezvous response";
        return false;
    }
    if (type == "ERROR") {
        *error = fields.empty() ? "Rendezvous rejected request" : fields[0];
        return false;
    }
    if (type != "CLIENTS" || fields.size() % kClientFields != 0) {
        *error = "Unexpected rendezvous response";
        return false;
    }
    for (size_t i = 0; i < fields.size(); i += kClientFields) {
        RendezvousPeerInfo info;
        info.peerId = fields[i];
        info.endpoint = fields[i + 1];
        std::string capabilityError;
        ParseTraversalModeSequence(fields[i + 2], &info.capabilities, &capabilityError);
        if (fields[i + 3] != kUnknownField) info.tunIp = fields[i + 3];
        ParseIdleSeconds(fields[i + 4], &info.idleSeconds);
        clients->push_back(std::move(info));
    }
    return true;
}

std::string FormatPeerCapabilities(const std::vector<TraversalMode>& capabilities) {
    if (capabilities.empty()) return "none";
    std::string output;
    for (const TraversalMode mode : capabilities) {
        if (!output.empty()) output += ", ";
        output += TraversalModeDisplayName(mode);
    }
    return output;
}
