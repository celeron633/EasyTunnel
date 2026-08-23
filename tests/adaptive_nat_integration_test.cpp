#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "adaptive_nat_traversal.h"
#include "nat_protocol.h"
#include "peer_selection.h"
#include "rendezvous/config.h"
#include "rendezvous/registry.h"
#include "rendezvous_client.h"
#include "util.h"

namespace {
int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}

void WriteUint16(uint8_t* data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value >> 8);
    data[1] = static_cast<uint8_t>(value);
}

void WriteUint32(uint8_t* data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value >> 24);
    data[1] = static_cast<uint8_t>(value >> 16);
    data[2] = static_cast<uint8_t>(value >> 8);
    data[3] = static_cast<uint8_t>(value);
}

UdpEndpoint FromSockaddr(const sockaddr_storage& address, socket_len_t len) {
    UdpEndpoint endpoint{};
    endpoint.addr = address;
    endpoint.addr_len = len;
    endpoint.family = address.ss_family;
    return endpoint;
}

socket_t OpenBoundSocket(const char* ip, UdpEndpoint* endpoint) {
    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSocket) return sock;
    UdpEndpoint bindEndpoint{};
    if (!ParseUdpEndpoint(ip, 0, &bindEndpoint)
        || bind(sock, reinterpret_cast<const sockaddr*>(&bindEndpoint.addr),
                bindEndpoint.addr_len) != 0) {
        CloseSocket(sock);
        return kInvalidSocket;
    }
    endpoint->addr_len = static_cast<socket_len_t>(sizeof(endpoint->addr));
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&endpoint->addr),
                    &endpoint->addr_len) != 0) {
        CloseSocket(sock);
        return kInvalidSocket;
    }
    endpoint->family = AF_INET;
    SetSocketRecvTimeoutMs(sock, 100);
    return sock;
}

std::vector<uint8_t> MakeBindingResponse(
        const uint8_t* request, const UdpEndpoint& mappedEndpoint) {
    std::vector<uint8_t> response(32, 0);
    WriteUint16(response.data(), 0x0101);
    WriteUint16(response.data() + 2, 12);
    WriteUint32(response.data() + 4, 0x2112A442);
    std::memcpy(response.data() + 8, request + 8, 12);
    WriteUint16(response.data() + 20, 0x0020);
    WriteUint16(response.data() + 22, 8);
    response[24] = 0;
    response[25] = 0x01;
    const auto* mapped =
        reinterpret_cast<const sockaddr_in*>(&mappedEndpoint.addr);
    WriteUint16(response.data() + 26,
        static_cast<uint16_t>(ntohs(mapped->sin_port) ^ 0x2112));
    const uint8_t* address =
        reinterpret_cast<const uint8_t*>(&mapped->sin_addr);
    const uint8_t cookie[4]{0x21, 0x12, 0xA4, 0x42};
    for (size_t i = 0; i < 4; ++i) {
        response[28 + i] = static_cast<uint8_t>(address[i] ^ cookie[i]);
    }
    return response;
}

uint16_t EndpointPort(const UdpEndpoint& endpoint) {
    return ntohs(reinterpret_cast<const sockaddr_in*>(
        &endpoint.addr)->sin_port);
}
}  // namespace

int main() {
#ifdef _WIN32
    WSADATA winsockData{};
    if (WSAStartup(MAKEWORD(2, 2), &winsockData) != 0) {
        std::cerr << "FAILED: Winsock initialization\n";
        return 1;
    }
#endif

    UdpEndpoint rendezvousEndpoint{};
    UdpEndpoint stunAEndpoint{};
    UdpEndpoint stunBEndpoint{};
    socket_t rendezvousSocket =
        OpenBoundSocket("127.0.0.1", &rendezvousEndpoint);
    socket_t stunA = OpenBoundSocket("127.0.0.1", &stunAEndpoint);
    socket_t stunB = OpenBoundSocket("127.0.0.2", &stunBEndpoint);
    Expect(rendezvousSocket != kInvalidSocket && stunA != kInvalidSocket
               && stunB != kInvalidSocket,
           "local rendezvous and two STUN sockets open");
    if (rendezvousSocket == kInvalidSocket || stunA == kInvalidSocket
        || stunB == kInvalidSocket) {
        CloseSocket(rendezvousSocket);
        CloseSocket(stunA);
        CloseSocket(stunB);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    RendezvousConfig rendezvousConfig;
    rendezvousConfig.clientTimeoutSeconds = 30;
    RendezvousRegistry registry(rendezvousSocket, rendezvousConfig);
    std::atomic<bool> servicesRunning{true};

    std::thread rendezvousThread([&] {
        std::vector<uint8_t> buffer(8192);
        while (servicesRunning.load()) {
            sockaddr_storage sourceAddress{};
            socket_len_t sourceLen =
                static_cast<socket_len_t>(sizeof(sourceAddress));
            const int received = recvfrom(rendezvousSocket,
                reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()), 0,
                reinterpret_cast<sockaddr*>(&sourceAddress), &sourceLen);
            if (received < 0) continue;
            std::string type;
            std::vector<std::string> fields;
            if (!ParseControlMessage(buffer.data(),
                    static_cast<size_t>(received), &type, &fields)) {
                continue;
            }
            registry.Handle(FromSockaddr(sourceAddress, sourceLen),
                            type, fields, std::chrono::steady_clock::now());
        }
    });

    auto runStun = [&](socket_t stunSocket) {
        std::vector<uint8_t> buffer(512);
        while (servicesRunning.load()) {
            sockaddr_storage sourceAddress{};
            socket_len_t sourceLen =
                static_cast<socket_len_t>(sizeof(sourceAddress));
            const int received = recvfrom(stunSocket,
                reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()), 0,
                reinterpret_cast<sockaddr*>(&sourceAddress), &sourceLen);
            if (received != 20 || buffer[0] != 0 || buffer[1] != 1) continue;
            const UdpEndpoint source = FromSockaddr(sourceAddress, sourceLen);
            const auto response = MakeBindingResponse(buffer.data(), source);
            sendto(stunSocket,
                reinterpret_cast<const char*>(response.data()),
                static_cast<int>(response.size()), 0,
                reinterpret_cast<const sockaddr*>(&source.addr), source.addr_len);
        }
    };
    std::thread stunAThread(runStun, stunA);
    std::thread stunBThread(runStun, stunB);

    Config aConfig;
    aConfig.rendezvous_addr = "127.0.0.1";
    aConfig.rendezvous_port = EndpointPort(rendezvousEndpoint);
    aConfig.room_id = "integration";
    aConfig.peer_id = "a";
    aConfig.target_peer_id = "b";
    aConfig.punch_timeout = 8;
    aConfig.stun_servers = {
        {"127.0.0.1", EndpointPort(stunAEndpoint)},
        {"127.0.0.2", EndpointPort(stunBEndpoint)},
    };
    Config bConfig = aConfig;
    bConfig.peer_id = "b";
    bConfig.target_peer_id.clear();

    socket_t aSocket = kInvalidSocket;
    socket_t bSocket = kInvalidSocket;
    UdpEndpoint aServer{};
    UdpEndpoint bServer{};
    std::string aError;
    std::string bError;
    Expect(OpenRendezvousSocket(
               aConfig, 100, &aSocket, &aServer, &aError)
               && OpenRendezvousSocket(
                   bConfig, 100, &bSocket, &bServer, &bError),
           "client control sockets open");

    std::atomic<bool> clientsRunning{true};
    UdpEndpoint aPeer{};
    UdpEndpoint bPeer{};
    std::string aPeerId;
    std::string bPeerId;
    std::vector<TraversalMode> aModes;
    std::vector<TraversalMode> bModes;
    NatPunchSession aSession;
    NatPunchSession bSession;
    bool aSelected = false;
    bool bSelected = false;
    std::thread bSelectThread([&] {
        bSelected = SelectPeer(bSocket, bConfig, bServer, clientsRunning,
            &bPeer, &bPeerId, &bModes, &bSession, &bError);
    });
    std::thread aSelectThread([&] {
        aSelected = SelectPeer(aSocket, aConfig, aServer, clientsRunning,
            &aPeer, &aPeerId, &aModes, &aSession, &aError);
    });
    aSelectThread.join();
    bSelectThread.join();
    Expect(aSelected && bSelected && aPeerId == "b" && bPeerId == "a",
           "two clients select each other through rendezvous");
    Expect(aSession.sessionId == bSession.sessionId
               && aSession.attemptId == bSession.attemptId
               && aSession.punchToken == bSession.punchToken
               && aSession.role == NatPunchRole::Initiator
               && bSession.role == NatPunchRole::Responder,
           "paired clients receive one complementary NAT session");

    bool aPunched = false;
    bool bPunched = false;
    if (aSelected && bSelected) {
        std::thread aPunchThread([&] {
            aPunched = PunchAdaptiveNat(&aSocket, aConfig, aServer,
                clientsRunning, aPeerId, aSession, &aPeer, &aError);
        });
        std::thread bPunchThread([&] {
            bPunched = PunchAdaptiveNat(&bSocket, bConfig, bServer,
                clientsRunning, bPeerId, bSession, &bPeer, &bError);
        });
        aPunchThread.join();
        bPunchThread.join();
    }
    Expect(aPunched && bPunched,
           "two clients complete STUN, barrier and PUNCH2");
    if (!aPunched) std::cerr << "A error: " << aError << '\n';
    if (!bPunched) std::cerr << "B error: " << bError << '\n';
    Expect(aPeer.family == AF_INET && bPeer.family == AF_INET,
           "winner sockets retain confirmed IPv4 peer endpoints");

    clientsRunning.store(false);
    CloseSocket(aSocket);
    CloseSocket(bSocket);
    servicesRunning.store(false);
    rendezvousThread.join();
    stunAThread.join();
    stunBThread.join();
    CloseSocket(rendezvousSocket);
    CloseSocket(stunA);
    CloseSocket(stunB);
#ifdef _WIN32
    WSACleanup();
#endif

    if (failures != 0) {
        std::cerr << failures << " adaptive NAT integration test(s) failed\n";
        return 1;
    }
    std::cout << "Adaptive NAT integration test passed\n";
    return 0;
}

