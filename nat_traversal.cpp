#include "nat_traversal.h"

#include <vector>

#include "nat_protocol.h"

namespace {
bool Send(socket_t sock, const UdpEndpoint& endpoint, const uint8_t* data, size_t len) {
    return sendto(sock, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
        reinterpret_cast<const sockaddr*>(&endpoint.addr), endpoint.addr_len) == static_cast<int>(len);
}
bool Send(socket_t sock, const UdpEndpoint& endpoint, const std::string& data) {
    return Send(sock, endpoint, reinterpret_cast<const uint8_t*>(data.data()), data.size());
}
}  // namespace

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
