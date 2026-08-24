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
                                    const std::string& matchedPeerId,
                                    const NatPunchSession& punchSession,
                                    const uint8_t* data, size_t len) {
    PeerControlResult result;
    std::string type;
    std::vector<std::string> fields;
    if (!ParseControlMessage(data, len, &type, &fields)) return result;
    result.handled = true;
    if (!SameUdpEndpoint(source, peer)) return result;

    if (type == "PUNCH" || type == "PUNCH_ACK") {
        const bool validPunch = fields.size() == 5
            && fields[0] == punchSession.sessionId
            && fields[1] == std::to_string(punchSession.attemptId)
            && fields[2] == matchedPeerId
            && IsSafeControlField(fields[3])
            && fields[4] == punchSession.punchToken;
        if (!validPunch) return result;
        if (type == "PUNCH") {
            Send(sock, peer, MakeControlMessage("PUNCH_ACK",
                {punchSession.sessionId, std::to_string(punchSession.attemptId),
                 cfg.peer_id, fields[3], punchSession.punchToken}));
        }
        result.peerSeen = true;
        return result;
    }

    if (type == "PEER_CLOSE") {
        const bool validClose = fields.size() == 4
            && fields[0] == punchSession.sessionId
            && fields[1] == std::to_string(punchSession.attemptId)
            && fields[2] == matchedPeerId
            && fields[3] == punchSession.punchToken;
        if (!validClose) return result;
        result.peerSeen = true;
        result.peerDisconnectRequested = true;
        return result;
    }

    if (fields.empty() || fields[0] != cfg.room_id) return result;
    if (type == "KEEPALIVE") {
        std::vector<std::string> ackFields{cfg.room_id, cfg.peer_id};
        if (fields.size() >= 3) ackFields.push_back(fields[2]);
        Send(sock, peer, MakeControlMessage("KEEPALIVE_ACK", ackFields));
    }
    if (type == "KEEPALIVE_ACK") {
        result.receivedKeepaliveAck = true;
        // Empty IDs are accepted by the engine for compatibility with older peers.
        result.keepaliveAckId = fields.size() >= 3 ? fields[2] : "";
    }
    if (type == "KEEPALIVE" || type == "KEEPALIVE_ACK" || type == "PADDING") {
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

bool SendPeerDisconnect(socket_t sock, const Config& cfg,
                        const UdpEndpoint& peer,
                        const NatPunchSession& punchSession,
                        uint8_t copies) {
    if (sock == kInvalidSocket || peer.family == AF_UNSPEC || copies == 0
        || !IsSafeControlField(cfg.peer_id)
        || !IsSafeControlField(punchSession.sessionId)
        || punchSession.attemptId == 0
        || punchSession.protocolVersion != kNatPunchProtocolVersionNumber
        || !IsSafeControlField(punchSession.punchToken)) {
        return false;
    }
    const std::string packet = MakeControlMessage("PEER_CLOSE",
        {punchSession.sessionId, std::to_string(punchSession.attemptId),
         cfg.peer_id, punchSession.punchToken});
    bool sent = false;
    for (uint8_t copy = 0; copy < copies; ++copy) {
        sent = Send(sock, peer, packet) || sent;
    }
    return sent;
}
