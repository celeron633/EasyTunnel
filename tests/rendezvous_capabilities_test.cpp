#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "nat_protocol.h"
#include "rendezvous_client.h"
#include "rendezvous/config.h"
#include "rendezvous/registry.h"
#include "util.h"

namespace {
int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}

socket_t OpenClient(UdpEndpoint* endpoint) {
    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSocket) return sock;
    UdpEndpoint local{};
    if (!ParseUdpEndpoint("127.0.0.1", 0, &local)
        || bind(sock, reinterpret_cast<const sockaddr*>(&local.addr),
                local.addr_len) != 0) {
        CloseSocket(sock);
        return kInvalidSocket;
    }
    endpoint->addr_len =
        static_cast<socket_len_t>(sizeof(endpoint->addr));
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&endpoint->addr),
                    &endpoint->addr_len) != 0) {
        CloseSocket(sock);
        return kInvalidSocket;
    }
    endpoint->family = AF_INET;
    SetSocketRecvTimeoutMs(sock, 200);
    return sock;
}

bool ReceiveControl(socket_t sock, std::string* type,
                    std::vector<std::string>* fields) {
    std::vector<uint8_t> buffer(2048);
    sockaddr_storage source{};
    socket_len_t sourceLen = static_cast<socket_len_t>(sizeof(source));
    const int received = recvfrom(sock, reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(buffer.size()), 0,
        reinterpret_cast<sockaddr*>(&source), &sourceLen);
    return received >= 0
        && ParseControlMessage(buffer.data(), static_cast<size_t>(received),
                               type, fields);
}

void Register(RendezvousRegistry* registry, const UdpEndpoint& endpoint,
              const std::string& room, const std::string& node,
              const std::string& capabilities) {
    registry->Handle(endpoint, "REG",
        {room, node, capabilities, ""},
        std::chrono::steady_clock::now());
}
}  // namespace

int main() {
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    socket_t serverSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    UdpEndpoint aEndpoint{};
    UdpEndpoint bEndpoint{};
    UdpEndpoint cEndpoint{};
    UdpEndpoint dEndpoint{};
    socket_t a = OpenClient(&aEndpoint);
    socket_t b = OpenClient(&bEndpoint);
    socket_t c = OpenClient(&cEndpoint);
    socket_t d = OpenClient(&dEndpoint);
    Expect(serverSocket != kInvalidSocket && a != kInvalidSocket
               && b != kInvalidSocket && c != kInvalidSocket
               && d != kInvalidSocket,
           "UDP sockets open");

    RendezvousConfig config;
    config.clientTimeoutSeconds = 60;
    RendezvousRegistry registry(serverSocket, config);
    std::string type;
    std::vector<std::string> fields;

    Register(&registry, aEndpoint, "common", "a",
             "ipv4_relay,nat_punch");
    Register(&registry, bEndpoint, "common", "b", "nat_punch");
    Expect(ReceiveControl(a, &type, &fields) && type == "REGISTERED",
           "initiator registers capabilities");
    Expect(ReceiveControl(b, &type, &fields) && type == "REGISTERED",
           "waiting peer registers capabilities");

    registry.Handle(aEndpoint, "CONNECT",
        {"common", "a", "b", "ipv4_relay,nat_punch", ""},
        std::chrono::steady_clock::now());
    Expect(ReceiveControl(a, &type, &fields) && type == "PEER"
               && fields.size() == 10
               && fields[3] == "nat_punch"
               && fields[4] == "nat_punch"
               && fields[7] == "initiator" && fields[8] == "2"
               && fields[9].size() == 64,
           "initiator receives peer capabilities and its preferred intersection");
    const std::string natSession = fields.size() == 10 ? fields[5] : "";
    const std::string natAttempt = fields.size() == 10 ? fields[6] : "";
    const std::string natPunchToken = fields.size() == 10 ? fields[9] : "";
    Expect(ReceiveControl(b, &type, &fields) && type == "PEER"
               && fields.size() == 10
               && fields[3] == "ipv4_relay,nat_punch"
               && fields[4] == "nat_punch"
               && fields[5] == natSession && fields[6] == natAttempt
               && fields[7] == "responder" && fields[8] == "2"
               && fields[9] == natPunchToken,
           "waiting peer receives the initiator's negotiated order");

    Config parserConfig;
    parserConfig.room_id = "common";
    parserConfig.peer_id = "a";
    parserConfig.target_peer_id = "b";
    RendezvousClient responseParser(parserConfig, aEndpoint);
    const std::string peerPacket = MakeControlMessage("PEER",
        {"203.0.113.20", "41000", "b", "nat_punch", "nat_punch",
         natSession, natAttempt, "initiator", "2", natPunchToken});
    const RendezvousEvent parsedPeer = responseParser.HandlePacket(
        aEndpoint, reinterpret_cast<const uint8_t*>(peerPacket.data()),
        peerPacket.size());
    Expect(parsedPeer.type == RendezvousEventType::Peer
               && parsedPeer.sessionId == natSession
               && parsedPeer.attemptId != 0
               && parsedPeer.natPunchRole == NatPunchRole::Initiator
               && parsedPeer.natPunchToken == natPunchToken,
           "client parses NAT session metadata from PEER");

    const std::string peerInfoPacket = MakeControlMessage("NAT_PEER_INFO",
        {natSession, natAttempt, "b", "port-dependent-regular",
         "203.0.113.20", "41000", "203.0.113.20", "41002", "-",
         "initiator", "2"});
    const RendezvousEvent parsedPeerInfo = responseParser.HandlePacket(
        aEndpoint, reinterpret_cast<const uint8_t*>(peerInfoPacket.data()),
        peerInfoPacket.size());
    Expect(parsedPeerInfo.type == RendezvousEventType::NatPeerInfo
               && parsedPeerInfo.peerId == "b"
               && FormatUdpEndpoint(parsedPeerInfo.natPeerInfo.mappedB)
                    == "203.0.113.20:41002",
           "client parses peer STUN mappings from NAT_PEER_INFO");

    UdpEndpoint aPunchEndpoint{};
    UdpEndpoint bPunchEndpoint{};
    socket_t aPunch = OpenClient(&aPunchEndpoint);
    socket_t bPunch = OpenClient(&bPunchEndpoint);
    Expect(aPunch != kInvalidSocket && bPunch != kInvalidSocket,
           "separate NAT punch sockets open");
    registry.Handle(aPunchEndpoint, "NAT_INFO",
        {"common", "a", "b", natSession, natAttempt, "2",
         "endpoint-independent", "198.51.100.10", "40000",
         "198.51.100.10", "40000", "-", ""},
        std::chrono::steady_clock::now());
    Expect(ReceiveControl(aPunch, &type, &fields) && type == "NAT_WAIT"
               && fields.size() == 2 && fields[0] == natSession
               && fields[1] == natAttempt,
           "first NAT report waits for its peer");

    registry.Handle(aPunchEndpoint, "NAT_ARMED",
        {"common", "a", "b", natSession, natAttempt, ""},
        std::chrono::steady_clock::now());
    Expect(ReceiveControl(aPunch, &type, &fields) && type == "ERROR"
               && fields.size() == 1
               && fields[0] == "invalid-nat-session",
           "out-of-order NAT_ARMED is rejected before both NAT reports");

    registry.Handle(aPunchEndpoint, "NAT_INFO",
        {"common", "a", "b", natSession, natAttempt, "2",
         "endpoint-independent", "198.51.100.10", "40000",
         "198.51.100.10", "40000", "-", ""},
        std::chrono::steady_clock::now());
    Expect(ReceiveControl(aPunch, &type, &fields) && type == "NAT_WAIT"
               && fields.size() == 2 && fields[0] == natSession
               && fields[1] == natAttempt,
           "duplicate NAT_INFO resends NAT_WAIT after response loss");

    registry.Handle(bPunchEndpoint, "NAT_INFO",
        {"common", "b", "a", natSession, natAttempt, "2",
         "port-dependent-regular", "203.0.113.20", "41000",
         "203.0.113.20", "41002", "-", ""},
        std::chrono::steady_clock::now());
    Expect(ReceiveControl(aPunch, &type, &fields)
               && type == "NAT_PEER_INFO" && fields.size() == 11
               && fields[0] == natSession && fields[1] == natAttempt
               && fields[2] == "b" && fields[3] == "port-dependent-regular"
               && fields[4] == "203.0.113.20" && fields[5] == "41000"
               && fields[7] == "41002" && fields[9] == "initiator",
           "initiator receives the responder's NAT report");
    Expect(ReceiveControl(bPunch, &type, &fields)
               && type == "NAT_PEER_INFO" && fields.size() == 11
               && fields[2] == "a" && fields[3] == "endpoint-independent"
               && fields[9] == "responder",
           "responder receives the initiator's NAT report");

    registry.Handle(aPunchEndpoint, "NAT_INFO",
        {"common", "a", "b", natSession, natAttempt, "2",
         "endpoint-independent", "198.51.100.10", "40000",
         "198.51.100.10", "40000", "-", ""},
        std::chrono::steady_clock::now());
    Expect(ReceiveControl(aPunch, &type, &fields)
               && type == "NAT_PEER_INFO" && fields[0] == natSession
               && fields[1] == natAttempt && fields[2] == "b",
           "duplicate NAT_INFO restores lost initiator peer information");
    Expect(ReceiveControl(bPunch, &type, &fields)
               && type == "NAT_PEER_INFO" && fields[0] == natSession
               && fields[1] == natAttempt && fields[2] == "a",
           "duplicate NAT_INFO keeps both peer views synchronized");

    registry.Handle(aPunchEndpoint, "NAT_ARMED",
        {"common", "a", "b", natSession, natAttempt, ""},
        std::chrono::steady_clock::now());
    Expect(ReceiveControl(aPunch, &type, &fields)
               && type == "NAT_ARMED_ACK",
           "first armed peer waits at the synchronization barrier");
    registry.Handle(aPunchEndpoint, "NAT_ARMED",
        {"common", "a", "b", natSession, natAttempt, ""},
        std::chrono::steady_clock::now());
    Expect(ReceiveControl(aPunch, &type, &fields)
               && type == "NAT_ARMED_ACK"
               && fields[0] == natSession && fields[1] == natAttempt,
           "duplicate NAT_ARMED restores a lost barrier acknowledgement");
    registry.Handle(bPunchEndpoint, "NAT_ARMED",
        {"common", "b", "a", natSession, natAttempt, ""},
        std::chrono::steady_clock::now());
    Expect(ReceiveControl(aPunch, &type, &fields) && type == "NAT_START"
               && fields[0] == natSession && fields[1] == natAttempt,
           "initiator receives synchronized NAT start");
    Expect(ReceiveControl(bPunch, &type, &fields) && type == "NAT_START"
               && fields[0] == natSession && fields[1] == natAttempt,
           "responder receives synchronized NAT start");

    registry.Handle(aPunchEndpoint, "NAT_ARMED",
        {"common", "a", "b", natSession, natAttempt, ""},
        std::chrono::steady_clock::now());
    Expect(ReceiveControl(aPunch, &type, &fields) && type == "NAT_START"
               && fields[0] == natSession && fields[1] == natAttempt,
           "duplicate NAT_ARMED restores a lost initiator NAT_START");
    Expect(ReceiveControl(bPunch, &type, &fields) && type == "NAT_START"
               && fields[0] == natSession && fields[1] == natAttempt,
           "duplicate NAT_ARMED retransmits NAT_START to both peers");

    registry.Handle(aEndpoint, "NAT_RETRY",
        {"common", "a", "b", natSession, natAttempt, ""},
        std::chrono::steady_clock::now());
    Expect(ReceiveControl(a, &type, &fields) && type == "NAT_RETRY_WAIT"
               && fields.size() == 2 && fields[0] == natSession
               && fields[1] == natAttempt,
           "first retry request waits for its peer");
    registry.Handle(aEndpoint, "NAT_RETRY",
        {"common", "a", "b", natSession, natAttempt, ""},
        std::chrono::steady_clock::now());
    Expect(ReceiveControl(a, &type, &fields) && type == "NAT_RETRY_WAIT"
               && fields.size() == 2 && fields[0] == natSession
               && fields[1] == natAttempt,
           "duplicate retry request keeps waiting on the same attempt");
    registry.Handle(bEndpoint, "NAT_RETRY",
        {"common", "b", "a", natSession, natAttempt, ""},
        std::chrono::steady_clock::now());
    std::string nextAttempt;
    std::string nextPunchToken;
    Expect(ReceiveControl(a, &type, &fields) && type == "NAT_ATTEMPT"
               && fields.size() == 5 && fields[0] == natSession
               && fields[1] != natAttempt && fields[2] == "initiator"
               && fields[3] == "2" && fields[4] != natPunchToken,
           "initiator receives a fresh retry attempt and token");
    if (fields.size() == 5) {
        nextAttempt = fields[1];
        nextPunchToken = fields[4];
    }
    Expect(ReceiveControl(b, &type, &fields) && type == "NAT_ATTEMPT"
               && fields.size() == 5 && fields[0] == natSession
               && fields[1] == nextAttempt && fields[2] == "responder"
               && fields[4] == nextPunchToken,
           "responder receives the same fresh retry attempt");

    const std::string attemptPacket = MakeControlMessage("NAT_ATTEMPT",
        {natSession, nextAttempt, "initiator", "2", nextPunchToken});
    const RendezvousEvent parsedAttempt = responseParser.HandlePacket(
        aEndpoint, reinterpret_cast<const uint8_t*>(attemptPacket.data()),
        attemptPacket.size());
    Expect(parsedAttempt.type == RendezvousEventType::NatAttempt
               && parsedAttempt.sessionId == natSession
               && parsedAttempt.attemptId > parsedPeer.attemptId
               && parsedAttempt.natPunchToken == nextPunchToken,
           "client parses synchronized retry metadata");

    registry.Handle(aEndpoint, "NAT_RETRY",
        {"common", "a", "b", natSession, natAttempt, ""},
        std::chrono::steady_clock::now());
    Expect(ReceiveControl(a, &type, &fields) && type == "NAT_ATTEMPT"
               && fields[1] == nextAttempt,
           "stale retry request resends the current attempt");

    registry.Handle(aPunchEndpoint, "NAT_ARMED",
        {"common", "a", "b", natSession, natAttempt, ""},
        std::chrono::steady_clock::now());
    Expect(ReceiveControl(aPunch, &type, &fields) && type == "ERROR"
               && fields.size() == 1
               && fields[0] == "invalid-nat-session",
           "old attempt NAT_ARMED is rejected after retry synchronization");

    registry.Handle(aPunchEndpoint, "NAT_INFO",
        {"common", "a", "b", natSession, natAttempt, "2",
         "endpoint-independent", "198.51.100.10", "40000",
         "198.51.100.10", "40000", "-", ""},
        std::chrono::steady_clock::now());
    Expect(ReceiveControl(aPunch, &type, &fields) && type == "ERROR"
               && fields.size() == 1
               && fields[0] == "invalid-nat-session",
           "old attempt NAT reports are rejected after retry synchronization");

    const std::string futureAttempt = std::to_string(
        std::stoull(nextAttempt) + 1);
    registry.Handle(aEndpoint, "NAT_RETRY",
        {"common", "a", "b", natSession, futureAttempt, ""},
        std::chrono::steady_clock::now());
    Expect(ReceiveControl(a, &type, &fields) && type == "ERROR"
               && fields.size() == 1
               && fields[0] == "invalid-nat-session",
           "future retry attempt is rejected instead of skipping synchronization");

    Register(&registry, cEndpoint, "incompatible", "c", "ipv6");
    Register(&registry, dEndpoint, "incompatible", "d", "ipv4_relay");
    Expect(ReceiveControl(c, &type, &fields) && type == "REGISTERED",
           "incompatible initiator registers");
    Expect(ReceiveControl(d, &type, &fields) && type == "REGISTERED",
           "incompatible waiting peer registers");

    registry.Handle(cEndpoint, "CONNECT",
        {"incompatible", "c", "d", "ipv6", ""},
        std::chrono::steady_clock::now());
    Expect(ReceiveControl(c, &type, &fields) && type == "ERROR"
               && fields.size() == 2
               && fields[0] == "no-common-traversal-mode"
               && fields[1] == "ipv4_relay",
           "initiator immediately receives incompatible capability error");
    Expect(!ReceiveControl(d, &type, &fields),
           "waiting peer remains waiting when capabilities are incompatible");

    registry.Handle(cEndpoint, "LIST", {"incompatible", ""},
                    std::chrono::steady_clock::now());
    Expect(ReceiveControl(c, &type, &fields) && type == "CLIENTS"
               && fields.size() == 10
               && fields[0] == "c" && fields[2] == "ipv6" && fields[3] == "-"
               && fields[5] == "d" && fields[7] == "ipv4_relay",
           "LIST reports each client with capabilities and TUN IP");
    const bool endpointsMatch = fields.size() == 10
        && fields[1] == FormatUdpEndpoint(cEndpoint)
        && fields[6] == FormatUdpEndpoint(dEndpoint);
    Expect(endpointsMatch, "LIST reports the public endpoint seen by the server");

    registry.Handle(cEndpoint, "LIST", {"common", ""},
                    std::chrono::steady_clock::now());
    Expect(ReceiveControl(c, &type, &fields) && type == "CLIENTS" && fields.empty(),
           "LIST keeps hiding paired clients");

    const auto snapshot = registry.Snapshot(std::chrono::steady_clock::now());
    bool incompatiblePeersRemainAvailable = false;
    for (const auto& room : snapshot) {
        if (room.roomId != "incompatible" || room.clients.size() != 2) continue;
        incompatiblePeersRemainAvailable =
            room.clients[0].pairedWith.empty()
            && room.clients[1].pairedWith.empty();
    }
    Expect(incompatiblePeersRemainAvailable,
           "incompatible peers are not marked as paired");

    CloseSocket(a);
    CloseSocket(b);
    CloseSocket(c);
    CloseSocket(d);
    CloseSocket(aPunch);
    CloseSocket(bPunch);
    CloseSocket(serverSocket);
#ifdef _WIN32
    WSACleanup();
#endif

    if (failures != 0) {
        std::cerr << failures << " rendezvous capability test(s) failed\n";
        return 1;
    }
    std::cout << "Rendezvous capability negotiation tests passed\n";
    return 0;
}
