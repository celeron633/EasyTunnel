#include "nat_traversal.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "log.h"
#include "nat4_traversal.h"
#include "nat_protocol.h"
#include "rendezvous_client.h"

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

bool DiscoverAndPunch(socket_t* sock, const Config& cfg,
                      const UdpEndpoint& server, const std::atomic<bool>& running,
                      UdpEndpoint* peer, std::string* error) {
    if (!ValidateRendezvousSession(cfg, error)) return false;
    RendezvousClient rendezvous(cfg, server);
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
        if (rendezvous.ResponseTimedOut(now, error)) return false;
        if ((!havePeer && rendezvous.HasResponded() && now >= selectionDeadline)
            || (havePeer && now >= punchDeadline)) break;
        if (havePeer && cfg.nat4_source_port_count > 0 && now >= legacyDeadline) {
            Log(LogLevel::Info, "Exact-port punch did not complete; switching to NAT4 socket pool");
            rendezvous.Unregister(*sock);
            CloseSocket(*sock);
            *sock = kInvalidSocket;
            return DiscoverAndPunchNat4(sock, cfg, rendezvous, running,
                                        expectedPeerId, punchDeadline, peer, error);
        }
        if (now >= nextProbe) {
            rendezvous.SendProbe(*sock);
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
            const int socketError = GetSocketError();
            if (IsRecvTimeout(socketError)) continue;
            if (rendezvous.HandleUnreachableError(socketError, error)) return false;
            *error = "UDP receive failed during NAT traversal";
            return false;
        }
        const UdpEndpoint source = FromSockaddr(sourceAddress, sourceLen);
        const RendezvousEvent rendezvousEvent = rendezvous.HandlePacket(
            source, buffer.data(), static_cast<size_t>(n));
        if (rendezvousEvent.type == RendezvousEventType::Error) {
            *error = rendezvousEvent.error;
            return false;
        }
        if (rendezvousEvent.type == RendezvousEventType::Registered
            || rendezvousEvent.type == RendezvousEventType::PeerUnavailable) {
            continue;
        }
        if (rendezvousEvent.type == RendezvousEventType::Peer) {
            if (!cfg.target_peer_id.empty()
                && rendezvousEvent.peerId != cfg.target_peer_id) continue;
            *peer = rendezvousEvent.peer;
            expectedPeerId = rendezvousEvent.peerId;
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
        if (rendezvousEvent.type != RendezvousEventType::None) continue;

        std::string type;
        std::vector<std::string> fields;
        if (!ParseControlMessage(buffer.data(), n, &type, &fields)) continue;
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
