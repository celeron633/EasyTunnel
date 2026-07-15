#include "nat_traversal.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "log.h"
#include "nat4_traversal.h"
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

bool DiscoverAndPunch(socket_t* sock, const Config& cfg,
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
    const auto selectionDeadline = std::chrono::steady_clock::now()
        + (cfg.target_peer_id.empty() ? std::chrono::hours(24 * 365)
                                      : std::chrono::seconds(cfg.punch_timeout));
    auto punchDeadline = (std::chrono::steady_clock::time_point::max)();
    auto legacyDeadline = (std::chrono::steady_clock::time_point::max)();
    auto nextProbe = std::chrono::steady_clock::time_point{};
    bool havePeer = false;
    std::string expectedPeerId;
    std::vector<uint8_t> buffer(2048);
    while (running.load()) {
        const auto now = std::chrono::steady_clock::now();
        if ((!havePeer && now >= selectionDeadline)
            || (havePeer && now >= punchDeadline)) break;
        if (havePeer && cfg.nat4_source_port_count > 0 && now >= legacyDeadline) {
            Log(LogLevel::Info, "Exact-port punch did not complete; switching to NAT4 socket pool");
            UnregisterRendezvous(*sock, cfg, server);
            CloseSocket(*sock);
            *sock = kInvalidSocket;
            return DiscoverAndPunchNat4(sock, cfg, server, running,
                                        expectedPeerId, punchDeadline, peer, error);
        }
        if (now >= nextProbe) {
            Send(*sock, server, reg);
            if (!cfg.target_peer_id.empty()) Send(*sock, server, connect);
            if (havePeer) {
                Send(*sock, *peer, MakeControlMessage(
                    "PUNCH", {cfg.room_id, cfg.peer_id}));
            }
            nextProbe = now + std::chrono::milliseconds(500);
        }
        sockaddr_storage sourceAddress{};
        socket_len_t sourceLen = static_cast<socket_len_t>(sizeof(sourceAddress));
        const int n = recvfrom(*sock, reinterpret_cast<char*>(buffer.data()),
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
            expectedPeerId = fields[2];
            if (!havePeer) {
                havePeer = true;
                punchDeadline = now + std::chrono::seconds(cfg.punch_timeout);
                legacyDeadline = cfg.nat4_source_port_count > 0
                    ? now + std::chrono::seconds(2) : punchDeadline;
                Log(LogLevel::Info, "Rendezvous supplied peer "
                    + FormatUdpEndpoint(*peer));
            }
            Send(*sock, *peer, MakeControlMessage(
                "PUNCH", {cfg.room_id, cfg.peer_id}));
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
                const std::string ack = MakeControlMessage(
                    "PUNCH_ACK", {cfg.room_id, cfg.peer_id});
                for (int repeat = 0; repeat < 5; ++repeat) Send(*sock, *peer, ack);
            }
            Log(LogLevel::Info, "NAT hole punching confirmed with " + FormatUdpEndpoint(*peer));
            return true;
        }
    }
    *error = havePeer ? "NAT punch timed out" : "Timed out waiting for selected peer";
    return false;
}

PeerControlResult HandlePeerControl(socket_t sock, const Config& cfg,
                                    const UdpEndpoint& peer, const UdpEndpoint& source,
                                    const uint8_t* data, size_t len) {
    PeerControlResult result;
    std::string type;
    std::vector<std::string> fields;
    if (!ParseControlMessage(data, len, &type, &fields)) return result;
    result.handled = true;
    if (!SameUdpEndpoint(source, peer) || fields.empty() || fields[0] != cfg.room_id) return result;
    if (type == "PUNCH" || type == "KEEPALIVE") {
        std::vector<std::string> ackFields{cfg.room_id, cfg.peer_id};
        if (type == "KEEPALIVE" && fields.size() >= 3) ackFields.push_back(fields[2]);
        Send(sock, peer, MakeControlMessage(
            type == "PUNCH" ? "PUNCH_ACK" : "KEEPALIVE_ACK", ackFields));
    }
    if (type == "KEEPALIVE_ACK") {
        result.receivedKeepaliveAck = true;
        // Empty IDs are accepted by the engine for compatibility with older peers.
        result.keepaliveAckId = fields.size() >= 3 ? fields[2] : "";
    }
    if (type == "PUNCH" || type == "PUNCH_ACK" || type == "KEEPALIVE"
        || type == "KEEPALIVE_ACK" || type == "PADDING") {
        result.peerSeen = true;
    }
    result.consumedDummyTraffic = type == "PADDING";
    return result;
}

bool SendPeerKeepalive(socket_t sock, const Config& cfg, const UdpEndpoint& peer,
                       const std::string& requestId) {
    std::vector<std::string> fields{cfg.room_id, cfg.peer_id};
    if (!requestId.empty()) fields.push_back(requestId);
    return Send(sock, peer, MakeControlMessage("KEEPALIVE", fields));
}

bool SendPeerDummyTraffic(socket_t sock, const Config& cfg, const UdpEndpoint& peer) {
    std::string packet = MakeControlMessage("PADDING", {cfg.room_id, cfg.peer_id, ""});
    if (packet.size() > kPeerDummyTrafficPacketSize) return false;
    packet.resize(kPeerDummyTrafficPacketSize, '0');
    return Send(sock, peer, packet);
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
