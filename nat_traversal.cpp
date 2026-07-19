#include "nat_traversal.h"

#include <algorithm>
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

const char* RendezvousEventName(RendezvousEventType type) {
    switch (type) {
        case RendezvousEventType::None: return "None";
        case RendezvousEventType::Registered: return "Registered";
        case RendezvousEventType::PeerUnavailable: return "PeerUnavailable";
        case RendezvousEventType::Peer: return "Peer";
        case RendezvousEventType::Nat4Wait: return "Nat4Wait";
        case RendezvousEventType::Nat4Peer: return "Nat4Peer";
        case RendezvousEventType::Error: return "Error";
        default: return "Unknown";
    }
}

std::string LocalSocketEndpoint(socket_t sock) {
    sockaddr_storage address{};
    socket_len_t len = static_cast<socket_len_t>(sizeof(address));
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&address), &len) != 0) {
        return "unknown (getsockname err=" + std::to_string(GetSocketError()) + ")";
    }
    return FormatUdpEndpoint(FromSockaddr(address, len));
}

long long MillisecondsRemaining(std::chrono::steady_clock::time_point deadline) {
    if (deadline == (std::chrono::steady_clock::time_point::max)()) return -1;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    return remaining > 0 ? static_cast<long long>(remaining) : 0LL;
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
    Log(LogLevel::Debug, "NAT traversal started: mode="
        + std::string(cfg.target_peer_id.empty() ? "wait" : "connect")
        + ", peer_id=" + cfg.peer_id
        + (cfg.target_peer_id.empty() ? "" : ", target_peer_id=" + cfg.target_peer_id)
        + ", punch_timeout_ms="
        + std::to_string(static_cast<unsigned long long>(cfg.punch_timeout) * 1000ULL)
        + ", nat4_pool_size=" + std::to_string(cfg.nat4_source_port_count));
    while (running.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (rendezvous.ResponseTimedOut(now, error)) {
            Log(LogLevel::Error, "NAT traversal aborted: " + *error);
            return false;
        }
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
            if (!rendezvous.SendProbe(*sock)) {
                Log(LogLevel::Warn, "Failed to send rendezvous probe to "
                    + FormatUdpEndpoint(server) + ". err="
                    + std::to_string(GetSocketError()));
            } else {
                Log(LogLevel::Debug, "Sent rendezvous probe; local="
                    + LocalSocketEndpoint(*sock) + ", server="
                    + FormatUdpEndpoint(server));
            }
            if (havePeer) {
                if (!Send(*sock, *peer, MakeControlMessage(
                        "PUNCH", {cfg.room_id, cfg.peer_id}))) {
                    Log(LogLevel::Debug, "Failed to send PUNCH to "
                        + FormatUdpEndpoint(*peer) + ". err="
                        + std::to_string(GetSocketError()));
                } else {
                    Log(LogLevel::Debug, "Sent PUNCH to "
                        + FormatUdpEndpoint(*peer) + "; deadline_ms="
                        + std::to_string(MillisecondsRemaining(punchDeadline)));
                }
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
            if (rendezvous.HandleUnreachableError(socketError, error)) {
                Log(LogLevel::Error, "NAT traversal receive failed before rendezvous response: "
                    + *error + ". err=" + std::to_string(socketError));
                return false;
            }
            if (IsUdpDestinationUnreachable(socketError)) {
                // ICMP port-unreachable is expected while the remote NAT mapping is
                // still being created. Windows reports it through the next recvfrom
                // as WSAECONNRESET (10054); retry instead of aborting the punch.
                Log(LogLevel::Debug, "Transient UDP unreachable during NAT traversal; "
                    "continuing. err=" + std::to_string(socketError)
                    + ", have_peer=" + (havePeer ? "true" : "false")
                    + ", deadline_ms="
                    + std::to_string(MillisecondsRemaining(
                        havePeer ? punchDeadline : selectionDeadline)));
                continue;
            }
            *error = "UDP receive failed during NAT traversal. err="
                + std::to_string(socketError);
            Log(LogLevel::Error, *error);
            return false;
        }
        const UdpEndpoint source = FromSockaddr(sourceAddress, sourceLen);
        const RendezvousEvent rendezvousEvent = rendezvous.HandlePacket(
            source, buffer.data(), static_cast<size_t>(n));
        Log(LogLevel::Debug, "NAT traversal RX: source="
            + FormatUdpEndpoint(source) + ", bytes=" + std::to_string(n)
            + ", rendezvous_event=" + RendezvousEventName(rendezvousEvent.type));
        if (rendezvousEvent.type == RendezvousEventType::Error) {
            *error = rendezvousEvent.error;
            Log(LogLevel::Error, "Rendezvous packet rejected: " + *error);
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
                Log(LogLevel::Info, "Rendezvous supplied peer id="
                    + expectedPeerId + ", endpoint=" + FormatUdpEndpoint(*peer)
                    + ", punch_deadline_ms="
                    + std::to_string(MillisecondsRemaining(punchDeadline)));
            } else {
                Log(LogLevel::Debug, "Rendezvous refreshed peer id="
                    + expectedPeerId + ", endpoint=" + FormatUdpEndpoint(*peer));
            }
            if (!Send(*sock, *peer, MakeControlMessage(
                    "PUNCH", {cfg.room_id, cfg.peer_id}))) {
                Log(LogLevel::Debug, "Failed to send immediate PUNCH to "
                    + FormatUdpEndpoint(*peer) + ". err="
                    + std::to_string(GetSocketError()));
            }
            continue;
        }
        if (rendezvousEvent.type != RendezvousEventType::None) continue;

        std::string type;
        std::vector<std::string> fields;
        if (!ParseControlMessage(buffer.data(), n, &type, &fields)) {
            Log(LogLevel::Debug, "Ignored non-control UDP packet during NAT traversal "
                "from " + FormatUdpEndpoint(source) + ", bytes=" + std::to_string(n));
            continue;
        }
        Log(LogLevel::Debug, "Received peer control type=" + type
            + " from " + FormatUdpEndpoint(source)
            + ", fields=" + std::to_string(fields.size()));
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
                int sentCount = 0;
                for (int repeat = 0; repeat < 5; ++repeat) {
                    if (Send(*sock, *peer, ack)) ++sentCount;
                }
                Log(LogLevel::Debug, "Sent PUNCH_ACK "
                    + std::to_string(sentCount) + "/5 times to "
                    + FormatUdpEndpoint(*peer));
            }
            Log(LogLevel::Info, "NAT hole punching confirmed with " + FormatUdpEndpoint(*peer));
            return true;
        }
        Log(LogLevel::Debug, "Ignored peer control type=" + type
            + ": have_peer=" + (havePeer ? "true" : "false")
            + ", same_ip=" + (havePeer && SameIpv4Address(source, *peer) ? "true" : "false")
            + ", room_match="
            + (fields.size() >= 1 && fields[0] == cfg.room_id ? "true" : "false")
            + ", peer_id_match="
            + (fields.size() >= 2 && fields[1] == expectedPeerId ? "true" : "false"));
    }
    if (!running.load()) {
        *error = "NAT traversal stopped by request";
    } else {
        *error = havePeer ? "NAT punch timed out" : "Timed out waiting for selected peer";
    }
    Log(running.load() ? LogLevel::Error : LogLevel::Debug,
        "NAT traversal finished without a connection: " + *error
        + ", have_peer=" + (havePeer ? "true" : "false"));
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
