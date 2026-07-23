#include "nat_traversal.h"

#include <chrono>
#include <vector>

#include "log.h"
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

bool PunchNat(socket_t* sock, const Config& cfg,
              const UdpEndpoint& server, const std::atomic<bool>& running,
              const std::string& matchedPeerId,
              UdpEndpoint* peer,
              std::string* error) {
    if (matchedPeerId.empty()) {
        *error = "Normal NAT traversal requires a selected peer";
        return false;
    }
    if (*sock == kInvalidSocket) {
        UdpEndpoint reopenedServer{};
        if (!OpenRendezvousSocket(cfg, 1000, sock, &reopenedServer, error)) return false;
    }
    RendezvousClient rendezvous(cfg, server);
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(cfg.punch_timeout);
    auto nextProbe = std::chrono::steady_clock::time_point{};
    bool havePeerEndpoint = peer->family == AF_INET;
    std::vector<uint8_t> buffer(2048);
    while (running.load() && std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextProbe) {
            rendezvous.SendProbe(*sock);
            if (havePeerEndpoint) {
                Send(*sock, *peer,
                     MakeControlMessage("PUNCH", {cfg.room_id, cfg.peer_id}));
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
            if (IsRecvTimeout(socketError) || IsUdpDestinationUnreachable(socketError)) continue;
            *error = "UDP receive failed during normal NAT traversal. err="
                + std::to_string(socketError);
            return false;
        }
        const UdpEndpoint source = FromSockaddr(sourceAddress, sourceLen);
        const RendezvousEvent event = rendezvous.HandlePacket(
            source, buffer.data(), static_cast<size_t>(n));
        if (event.type == RendezvousEventType::Error) {
            *error = event.error;
            return false;
        }
        if (event.type == RendezvousEventType::Peer
            && event.peerId == matchedPeerId) {
            *peer = event.peer;
            havePeerEndpoint = true;
            continue;
        }
        if (event.type != RendezvousEventType::None) continue;
        std::string type;
        std::vector<std::string> fields;
        if (!ParseControlMessage(buffer.data(), static_cast<size_t>(n),
                                 &type, &fields)) continue;
        if (!havePeerEndpoint || !SameIpv4Address(source, *peer) || fields.size() < 2
            || fields[0] != cfg.room_id || fields[1] != matchedPeerId
            || (type != "PUNCH" && type != "PUNCH_ACK")) continue;
        *peer = source;
        if (type == "PUNCH") {
            const std::string ack = MakeControlMessage(
                "PUNCH_ACK", {cfg.room_id, cfg.peer_id});
            for (int repeat = 0; repeat < 5; ++repeat) Send(*sock, *peer, ack);
        }
        Log(LogLevel::Info, "Normal NAT traversal confirmed with "
            + FormatUdpEndpoint(*peer));
        return true;
    }
    *error = running.load() ? "Normal NAT traversal timed out"
                            : "Normal NAT traversal stopped";
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
