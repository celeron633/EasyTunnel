#pragma once

#include <atomic>
#include <string>
#include "config.h"
#include "rendezvous_client.h"
#include "util.h"

constexpr size_t kPeerDummyTrafficPacketSize = 1024;

// Describes how an incoming UDP packet was handled by HandlePeerControl.
struct PeerControlResult {
    // True when the packet is a control message and must not enter the data plane.
    bool handled = false;
    // True when the message proves the configured peer is still reachable.
    bool peerSeen = false;
    // True when a PADDING packet should be included in receive statistics.
    bool consumedDummyTraffic = false;
    // True when the message is a KEEPALIVE_ACK.
    bool receivedKeepaliveAck = false;
    // Echoed heartbeat request ID; empty for ACKs from older peers.
    std::string keepaliveAckId;
};

PeerControlResult HandlePeerControl(socket_t sock, const Config& cfg,
                                    const UdpEndpoint& peer, const UdpEndpoint& source,
                                    const std::string& matchedPeerId,
                                    const NatPunchSession& punchSession,
                                    const uint8_t* data, size_t len);

bool SendPeerKeepalive(socket_t sock, const Config& cfg, const UdpEndpoint& peer,
                       const std::string& requestId = "");
bool SendPeerDummyTraffic(socket_t sock, const Config& cfg, const UdpEndpoint& peer);
