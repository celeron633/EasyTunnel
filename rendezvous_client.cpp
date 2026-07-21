#include "rendezvous_client.h"

#include <chrono>
#include <limits>
#include <utility>

#include "nat_protocol.h"

namespace {
constexpr auto kRendezvousResponseTimeout = std::chrono::seconds(5);
constexpr const char* kNoResponseError = "Rendezvous server did not respond";

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

bool ParseRound(const std::string& text, uint32_t* round) {
    try {
        size_t consumed = 0;
        const unsigned long value = std::stoul(text, &consumed);
        if (consumed != text.size()
            || value > (std::numeric_limits<uint32_t>::max)()) return false;
        *round = static_cast<uint32_t>(value);
        return true;
    } catch (...) {
        return false;
    }
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

RendezvousEvent InvalidResponse() {
    RendezvousEvent event;
    event.type = RendezvousEventType::Error;
    event.error = "Invalid rendezvous response";
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
    bool sent = Send(sock, server_, MakeControlMessage("REG",
        {config_.room_id, config_.peer_id, config_.auth_token}));
    if (!config_.target_peer_id.empty()) {
        sent = Send(sock, server_, MakeControlMessage("CONNECT",
            {config_.room_id, config_.peer_id,
             config_.target_peer_id, config_.auth_token})) && sent;
    }
    return sent;
}

bool RendezvousClient::SendNat4Join(socket_t sock,
                                    const std::string& expectedPeerId,
                                    uint32_t round) const {
    return Send(sock, server_, MakeControlMessage("NAT4_JOIN",
        {config_.room_id, config_.peer_id, expectedPeerId,
         std::to_string(round), config_.auth_token}));
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
        event.type = RendezvousEventType::Registered;
        return event;
    }
    if (type == "ERROR") {
        if (!fields.empty() && fields[0] == "peer-not-found") {
            event.type = RendezvousEventType::PeerUnavailable;
        } else {
            event.type = RendezvousEventType::Error;
            event.error = fields.empty()
                ? "Rendezvous rejected request"
                : "Rendezvous error: " + fields[0];
        }
        return event;
    }
    if (type == "PEER") {
        if (fields.size() != 3
            || !ParsePeer(fields, &event.peer, &event.peerId)) {
            return InvalidResponse();
        }
        event.type = RendezvousEventType::Peer;
        return event;
    }
    if (type == "NAT4_WAIT") {
        if (fields.size() != 1 || !ParseRound(fields[0], &event.round)) {
            return InvalidResponse();
        }
        event.type = RendezvousEventType::Nat4Wait;
        return event;
    }
    if (type == "NAT4_PEER") {
        if (fields.size() != 4
            || !ParsePeer(fields, &event.peer, &event.peerId)
            || !ParseRound(fields[3], &event.round)) {
            return InvalidResponse();
        }
        event.type = RendezvousEventType::Nat4Peer;
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
                           std::vector<std::string>* clients, std::string* error) {
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
    std::vector<uint8_t> buffer(4096);
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
    if (type != "CLIENTS") {
        *error = "Unexpected rendezvous response";
        return false;
    }
    *clients = std::move(fields);
    return true;
}
