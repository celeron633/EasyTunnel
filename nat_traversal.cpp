#include "nat_traversal.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "log.h"
#include "nat_protocol.h"

namespace {
bool Send(socket_t sock, const UdpEndpoint& endpoint, const uint8_t* data, size_t len) {
    return sendto(sock, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
        reinterpret_cast<const sockaddr*>(&endpoint.addr), endpoint.addr_len) == static_cast<int>(len);
}
bool Send(socket_t sock, const UdpEndpoint& endpoint, const std::string& data) {
    return Send(sock, endpoint, reinterpret_cast<const uint8_t*>(data.data()), data.size());
}
UdpEndpoint FromSockaddr(const sockaddr_storage& address, socket_len_t len) {
    UdpEndpoint out{};
    out.addr = address;
    out.addr_len = len;
    out.family = address.ss_family;
    return out;
}

bool SameIpv4Address(const UdpEndpoint& a, const UdpEndpoint& b) {
    if (a.family != AF_INET || b.family != AF_INET) return false;
    const auto* aa = reinterpret_cast<const sockaddr_in*>(&a.addr);
    const auto* ba = reinterpret_cast<const sockaddr_in*>(&b.addr);
    return aa->sin_addr.s_addr == ba->sin_addr.s_addr;
}

bool EndpointWithPortOffset(const UdpEndpoint& base, uint16_t offset,
                            UdpEndpoint* endpoint) {
    if (base.family != AF_INET) return false;
    const auto* address = reinterpret_cast<const sockaddr_in*>(&base.addr);
    const uint32_t port = static_cast<uint32_t>(ntohs(address->sin_port)) + offset;
    if (port == 0 || port > 65535) return false;
    *endpoint = base;
    reinterpret_cast<sockaddr_in*>(&endpoint->addr)->sin_port = htons(
        static_cast<uint16_t>(port));
    return true;
}

void SendPunchCandidates(socket_t sock, const Config& cfg,
                         const UdpEndpoint& rendezvousPeer) {
    const std::string punch = MakeControlMessage("PUNCH", {cfg.room_id, cfg.peer_id});
    for (uint32_t offset = 0; offset <= cfg.nat4_max_port_offset; ++offset) {
        UdpEndpoint candidate{};
        if (EndpointWithPortOffset(rendezvousPeer, static_cast<uint16_t>(offset),
                                   &candidate)) {
            Send(sock, candidate, punch);
        }
    }
}
}  // namespace

bool OpenNatUdpSocket(const Config& cfg, int recvTimeoutMs, socket_t* sock,
                      UdpEndpoint* server, std::string* error) {
    if (!ResolveUdpEndpoint(cfg.rendezvous_addr, cfg.rendezvous_port, AF_INET,
                            server, error)) return false;
    socket_t opened = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (opened == kInvalidSocket) {
        *error = "Cannot create IPv4 UDP socket. err=" + std::to_string(GetSocketError());
        return false;
    }
    SetSocketRecvTimeoutMs(opened, recvTimeoutMs);
    *sock = opened;
    return true;
}

bool DiscoverAndPunch(socket_t sock, const Config& cfg,
                      const UdpEndpoint& server, const std::atomic<bool>& running,
                      UdpEndpoint* peer, std::string* error) {
    if (!IsSafeControlField(cfg.room_id) || !IsSafeControlField(cfg.peer_id)
        || (!cfg.target_peer_id.empty() && !IsSafeControlField(cfg.target_peer_id))
        || (!cfg.auth_token.empty() && !IsSafeControlField(cfg.auth_token))) {
        *error = "room_id/peer_id/target_peer_id/auth_token is invalid";
        return false;
    }
    const std::string reg = MakeControlMessage("REG",
        {cfg.room_id, cfg.peer_id, cfg.auth_token});
    const std::string connect = MakeControlMessage("CONNECT",
        {cfg.room_id, cfg.peer_id, cfg.target_peer_id, cfg.auth_token});
    const auto deadline = std::chrono::steady_clock::now()
        + (cfg.target_peer_id.empty() ? std::chrono::hours(24 * 365)
                                      : std::chrono::seconds(cfg.punch_timeout));
    auto nextProbe = std::chrono::steady_clock::time_point{};
    bool havePeer = false;
    std::string expectedPeerId;
    std::vector<uint8_t> buffer(2048);
    while (running.load() && std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextProbe) {
            Send(sock, server, reg);
            if (!cfg.target_peer_id.empty()) Send(sock, server, connect);
            if (havePeer) {
                SendPunchCandidates(sock, cfg, *peer);
            }
            nextProbe = now + std::chrono::milliseconds(500);
        }
        sockaddr_storage sourceAddress{};
        socket_len_t sourceLen = static_cast<socket_len_t>(sizeof(sourceAddress));
        const int n = recvfrom(sock, reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&sourceAddress), &sourceLen);
        if (n < 0) {
            if (IsRecvTimeout(GetSocketError())) continue;
            *error = "UDP receive failed during NAT traversal";
            return false;
        }
        const UdpEndpoint source = FromSockaddr(sourceAddress, sourceLen);
        std::string type;
        std::vector<std::string> fields;
        if (!ParseControlMessage(buffer.data(), n, &type, &fields)) continue;
        if (SameUdpEndpoint(source, server) && type == "ERROR" && !fields.empty()
            && fields[0] != "peer-not-found") {
            *error = "Rendezvous error: " + fields[0];
            return false;
        }
        if (SameUdpEndpoint(source, server) && type == "PEER" && fields.size() == 3) {
            if (!cfg.target_peer_id.empty() && fields[2] != cfg.target_peer_id) continue;
            unsigned long port = 0;
            try { port = std::stoul(fields[1]); } catch (...) { continue; }
            UdpEndpoint candidate{};
            if (port == 0 || port > 65535
                || !ParseUdpEndpoint(fields[0], static_cast<uint16_t>(port), &candidate)) continue;
            *peer = candidate;
            havePeer = true;
            expectedPeerId = fields[2];
            Log(LogLevel::Info, "Rendezvous supplied peer " + FormatUdpEndpoint(*peer));
            if (cfg.nat4_max_port_offset > 0) {
                Log(LogLevel::Info, "NAT4 port prediction enabled: scanning +0..+"
                    + std::to_string(cfg.nat4_max_port_offset));
            }
            SendPunchCandidates(sock, cfg, *peer);
            continue;
        }
        if (havePeer && SameIpv4Address(source, *peer) && fields.size() >= 2
            && fields[0] == cfg.room_id && fields[1] == expectedPeerId
            && (type == "PUNCH" || type == "PUNCH_ACK")) {
            if (!SameUdpEndpoint(source, *peer)) {
                Log(LogLevel::Info, "NAT port prediction matched peer "
                    + FormatUdpEndpoint(source));
                *peer = source;
            }
            if (type == "PUNCH") {
                Send(sock, *peer, MakeControlMessage("PUNCH_ACK", {cfg.room_id, cfg.peer_id}));
            }
            Log(LogLevel::Info, "NAT hole punching confirmed with " + FormatUdpEndpoint(*peer));
            return true;
        }
    }
    *error = havePeer ? "NAT punch timed out" : "Timed out waiting for selected peer";
    return false;
}

bool HandlePeerControl(socket_t sock, const Config& cfg,
                       const UdpEndpoint& peer, const UdpEndpoint& source,
                       const uint8_t* data, size_t len,
                       std::chrono::steady_clock::time_point* lastPeerSeen) {
    std::string type;
    std::vector<std::string> fields;
    if (!ParseControlMessage(data, len, &type, &fields)) return false;
    if (!SameUdpEndpoint(source, peer) || fields.empty() || fields[0] != cfg.room_id) return true;
    if (type == "PUNCH" || type == "KEEPALIVE") {
        Send(sock, peer, MakeControlMessage(type == "PUNCH" ? "PUNCH_ACK" : "KEEPALIVE_ACK",
                                            {cfg.room_id, cfg.peer_id}));
    }
    if (type == "PUNCH" || type == "PUNCH_ACK" || type == "KEEPALIVE"
        || type == "KEEPALIVE_ACK") {
        *lastPeerSeen = std::chrono::steady_clock::now();
    }
    return true;
}

bool SendPeerKeepalive(socket_t sock, const Config& cfg, const UdpEndpoint& peer) {
    return Send(sock, peer, MakeControlMessage("KEEPALIVE", {cfg.room_id, cfg.peer_id}));
}

void UnregisterRendezvous(socket_t sock, const Config& cfg, const UdpEndpoint& server) {
    if (sock == kInvalidSocket) return;
    Send(sock, server, MakeControlMessage("UNREG",
        {cfg.room_id, cfg.peer_id, cfg.auth_token}));
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
        *error = "Rendezvous server did not respond";
        return false;
    }
    const UdpEndpoint source = FromSockaddr(sourceAddress, sourceLen);
    std::string type;
    std::vector<std::string> fields;
    if (!SameUdpEndpoint(source, server)
        || !ParseControlMessage(buffer.data(), n, &type, &fields)) {
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
