#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "nat_protocol.h"
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
             "ipv4_relay,nat4,nat");
    Register(&registry, bEndpoint, "common", "b", "nat,nat4");
    Expect(ReceiveControl(a, &type, &fields) && type == "REGISTERED",
           "initiator registers capabilities");
    Expect(ReceiveControl(b, &type, &fields) && type == "REGISTERED",
           "waiting peer registers capabilities");

    registry.Handle(aEndpoint, "CONNECT",
        {"common", "a", "b", "ipv4_relay,nat4,nat", ""},
        std::chrono::steady_clock::now());
    Expect(ReceiveControl(a, &type, &fields) && type == "PEER"
               && fields.size() == 5
               && fields[3] == "nat,nat4"
               && fields[4] == "nat4,nat",
           "initiator receives peer capabilities and its preferred intersection");
    Expect(ReceiveControl(b, &type, &fields) && type == "PEER"
               && fields.size() == 5
               && fields[3] == "ipv4_relay,nat4,nat"
               && fields[4] == "nat4,nat",
           "waiting peer receives the initiator's negotiated order");

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
