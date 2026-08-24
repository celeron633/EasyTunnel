#include <algorithm>
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
#include "nat_traversal.h"
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

bool WaitForNatPeerInfo(socket_t sock, RendezvousClient* rendezvous,
                        const NatPunchSession& session,
                        const std::string& peerId) {
    std::vector<uint8_t> buffer(2048);
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        sockaddr_storage sourceAddress{};
        socket_len_t sourceLen =
            static_cast<socket_len_t>(sizeof(sourceAddress));
        const int received = recvfrom(sock,
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&sourceAddress), &sourceLen);
        if (received < 0) {
            const int socketError = GetSocketError();
            if (IsRecvTimeout(socketError)
                || IsUdpDestinationUnreachable(socketError)) {
                continue;
            }
            return false;
        }
        const RendezvousEvent event = rendezvous->HandlePacket(
            FromSockaddr(sourceAddress, sourceLen), buffer.data(),
            static_cast<size_t>(received));
        if (event.type == RendezvousEventType::NatPeerInfo
            && event.sessionId == session.sessionId
            && event.attemptId == session.attemptId
            && event.peerId == peerId) {
            return true;
        }
    }
    return false;
}

bool WaitForListedTunIp(const Config& config, const std::string& expectedTunIp) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<RendezvousPeerInfo> clients;
        std::string error;
        if (ListRendezvousClients(
                config.rendezvous_addr, config.rendezvous_port,
                config.room_id, config.auth_token, &clients, &error)) {
            for (const RendezvousPeerInfo& client : clients) {
                if (client.peerId == config.peer_id
                    && client.tunIp == expectedTunIp) {
                    return true;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

bool WaitForPeersUnlisted(const Config& config,
                          const std::vector<std::string>& peerIds) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<RendezvousPeerInfo> clients;
        std::string error;
        if (ListRendezvousClients(
                config.rendezvous_addr, config.rendezvous_port,
                config.room_id, config.auth_token, &clients, &error)) {
            bool found = false;
            for (const RendezvousPeerInfo& client : clients) {
                if (std::find(peerIds.begin(), peerIds.end(), client.peerId)
                    != peerIds.end()) {
                    found = true;
                    break;
                }
            }
            if (!found) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

struct InterruptedPunchResult {
    bool setup = false;
    bool peerInfoReceived = false;
    bool punched = false;
    NatPunchAttemptResult attempt;
    std::string error;
};

enum class InterruptedPunchMode {
    CancelAtBarrier,
    PunchTimeout,
    BarrierTimeout,
};

InterruptedPunchResult RunInterruptedRandomReceiver(
        Config easyConfig, Config randomConfig,
        const UdpEndpoint& rendezvousEndpoint, InterruptedPunchMode mode,
        int sequence) {
    InterruptedPunchResult result;
    easyConfig.room_id = "interrupt-" + std::to_string(sequence);
    easyConfig.peer_id = "easy";
    easyConfig.target_peer_id = "random";
    randomConfig.room_id = easyConfig.room_id;
    randomConfig.peer_id = "random";
    randomConfig.target_peer_id = "easy";
    randomConfig.punch_timeout = mode == InterruptedPunchMode::CancelAtBarrier
        ? 5 : mode == InterruptedPunchMode::PunchTimeout ? 2 : 30;

    socket_t easyControl = kInvalidSocket;
    socket_t randomControl = kInvalidSocket;
    socket_t randomPunch = kInvalidSocket;
    UdpEndpoint easyServer{};
    UdpEndpoint randomServer{};
    std::string easyError;
    if (!OpenRendezvousSocket(
            easyConfig, 100, &easyControl, &easyServer, &easyError)
        || !OpenRendezvousSocket(
            randomConfig, 100, &randomControl, &randomServer,
            &result.error)) {
        CloseSocket(easyControl);
        CloseSocket(randomControl);
        return result;
    }

    std::atomic<bool> running{true};
    UdpEndpoint easyPeer{};
    UdpEndpoint randomPeer{};
    std::string easyPeerId;
    std::string randomPeerId;
    std::vector<TraversalMode> easyModes;
    std::vector<TraversalMode> randomModes;
    NatPunchSession easySession;
    NatPunchSession randomSession;
    bool easySelected = false;
    bool randomSelected = false;
    std::thread easySelectThread([&] {
        easySelected = SelectPeer(easyControl, easyConfig, easyServer,
            running, &easyPeer, &easyPeerId, &easyModes,
            &easySession, &easyError);
    });
    std::thread randomSelectThread([&] {
        randomSelected = SelectPeer(randomControl, randomConfig,
            randomServer, running, &randomPeer, &randomPeerId,
            &randomModes, &randomSession, &result.error);
    });
    easySelectThread.join();
    randomSelectThread.join();
    if (!easySelected || !randomSelected) {
        running.store(false);
        CloseSocket(easyControl);
        CloseSocket(randomControl);
        return result;
    }

    UdpEndpoint easyPunchEndpoint{};
    socket_t easyPunch = OpenBoundSocket("127.0.0.1", &easyPunchEndpoint);
    if (easyPunch == kInvalidSocket) {
        CloseSocket(easyControl);
        CloseSocket(randomControl);
        return result;
    }
    result.setup = true;
    RendezvousClient easyRendezvous(easyConfig, rendezvousEndpoint);
    std::thread randomPunchThread([&] {
        const uint16_t attemptNumber =
            mode == InterruptedPunchMode::BarrierTimeout ? 1 : 3;
        result.punched = PunchAdaptiveNat(randomControl, &randomPunch,
            randomConfig,
            randomServer, running, randomPeerId, randomSession,
            &randomPeer, &result.error, &result.attempt, attemptNumber);
    });

    if (easyRendezvous.SendNatInfo(easyControl, easyPeerId,
            easySession.sessionId, easySession.attemptId,
            NatMappingBehaviorName(NatMappingBehavior::EndpointIndependent),
            easyPunchEndpoint, easyPunchEndpoint, "-")) {
        result.peerInfoReceived = WaitForNatPeerInfo(
            easyControl, &easyRendezvous, easySession, easyPeerId);
    }
    if (mode == InterruptedPunchMode::PunchTimeout
        && result.peerInfoReceived) {
        easyRendezvous.SendNatArmed(easyControl, easyPeerId,
            easySession.sessionId, easySession.attemptId);
    }
    if (mode == InterruptedPunchMode::CancelAtBarrier) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        running.store(false);
    }
    randomPunchThread.join();

    running.store(false);
    UnregisterRendezvous(easyControl, easyConfig, easyServer);
    UnregisterRendezvous(randomControl, randomConfig, randomServer);
    CloseSocket(easyPunch);
    CloseSocket(randomPunch);
    CloseSocket(easyControl);
    CloseSocket(randomControl);
    return result;
}

struct PairedPunchResult {
    bool setup = false;
    bool selected = false;
    bool firstPunched = false;
    bool secondPunched = false;
    NatPunchAttemptResult firstAttempt;
    NatPunchAttemptResult secondAttempt;
    std::string firstError;
    std::string secondError;
};

PairedPunchResult RunPairedPunch(Config firstConfig, Config secondConfig,
                                 uint16_t attemptNumber) {
    PairedPunchResult result;
    socket_t firstSocket = kInvalidSocket;
    socket_t secondSocket = kInvalidSocket;
    socket_t firstPunch = kInvalidSocket;
    socket_t secondPunch = kInvalidSocket;
    UdpEndpoint firstServer{};
    UdpEndpoint secondServer{};
    if (!OpenRendezvousSocket(firstConfig, 100, &firstSocket,
                              &firstServer, &result.firstError)
        || !OpenRendezvousSocket(secondConfig, 100, &secondSocket,
                                 &secondServer, &result.secondError)) {
        CloseSocket(firstSocket);
        CloseSocket(secondSocket);
        return result;
    }
    result.setup = true;

    std::atomic<bool> running{true};
    UdpEndpoint firstPeer{};
    UdpEndpoint secondPeer{};
    std::string firstPeerId;
    std::string secondPeerId;
    std::vector<TraversalMode> firstModes;
    std::vector<TraversalMode> secondModes;
    NatPunchSession firstSession;
    NatPunchSession secondSession;
    bool firstSelected = false;
    bool secondSelected = false;
    std::thread firstSelectThread([&] {
        firstSelected = SelectPeer(firstSocket, firstConfig, firstServer,
            running, &firstPeer, &firstPeerId, &firstModes,
            &firstSession, &result.firstError);
    });
    std::thread secondSelectThread([&] {
        secondSelected = SelectPeer(secondSocket, secondConfig,
            secondServer, running, &secondPeer, &secondPeerId,
            &secondModes, &secondSession, &result.secondError);
    });
    firstSelectThread.join();
    secondSelectThread.join();
    result.selected = firstSelected && secondSelected;
    if (result.selected) {
        std::thread firstPunchThread([&] {
            result.firstPunched = PunchAdaptiveNat(firstSocket, &firstPunch,
                firstConfig, firstServer, running, firstPeerId,
                firstSession, &firstPeer, &result.firstError,
                &result.firstAttempt, attemptNumber);
        });
        std::thread secondPunchThread([&] {
            result.secondPunched = PunchAdaptiveNat(secondSocket, &secondPunch,
                secondConfig, secondServer, running, secondPeerId,
                secondSession, &secondPeer, &result.secondError,
                &result.secondAttempt, attemptNumber);
        });
        firstPunchThread.join();
        secondPunchThread.join();
    }

    running.store(false);
    UnregisterRendezvous(firstSocket, firstConfig, firstServer);
    UnregisterRendezvous(secondSocket, secondConfig, secondServer);
    CloseSocket(firstPunch);
    CloseSocket(secondPunch);
    CloseSocket(firstSocket);
    CloseSocket(secondSocket);
    return result;
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

    Expect(LimitNatPunchBarrierWaitMs(30000) == 8000
               && LimitNatPunchBarrierWaitMs(2500) == 2500
               && LimitNatPunchBarrierWaitMs(0) == 0,
           "barrier wait is capped without extending attempt time");

    UdpEndpoint rendezvousEndpoint{};
    UdpEndpoint stunAEndpoint{};
    UdpEndpoint stunBEndpoint{};
    UdpEndpoint stunRandomEndpoint{};
    UdpEndpoint stunRegularEndpoint{};
    socket_t rendezvousSocket =
        OpenBoundSocket("127.0.0.1", &rendezvousEndpoint);
    socket_t stunA = OpenBoundSocket("127.0.0.1", &stunAEndpoint);
    socket_t stunB = OpenBoundSocket("127.0.0.2", &stunBEndpoint);
    socket_t stunRandom = OpenBoundSocket(
        "127.0.0.3", &stunRandomEndpoint);
    socket_t stunRegular = OpenBoundSocket(
        "127.0.0.4", &stunRegularEndpoint);
    Expect(rendezvousSocket != kInvalidSocket && stunA != kInvalidSocket
               && stunB != kInvalidSocket
               && stunRandom != kInvalidSocket
               && stunRegular != kInvalidSocket,
           "local rendezvous and STUN sockets open");
    if (rendezvousSocket == kInvalidSocket || stunA == kInvalidSocket
        || stunB == kInvalidSocket || stunRandom == kInvalidSocket
        || stunRegular == kInvalidSocket) {
        CloseSocket(rendezvousSocket);
        CloseSocket(stunA);
        CloseSocket(stunB);
        CloseSocket(stunRandom);
        CloseSocket(stunRegular);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    RendezvousConfig rendezvousConfig;
    rendezvousConfig.clientTimeoutSeconds = 30;
    RendezvousRegistry registry(rendezvousSocket, rendezvousConfig);
    std::atomic<bool> servicesRunning{true};
    std::atomic<int> natArmedDropsRemaining{0};

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
            if (type == "NAT_ARMED"
                && natArmedDropsRemaining.load() > 0) {
                natArmedDropsRemaining.fetch_sub(1);
                continue;
            }
            registry.Handle(FromSockaddr(sourceAddress, sourceLen),
                            type, fields, std::chrono::steady_clock::now());
        }
    });

    auto runStun = [&](socket_t stunSocket, int mappedPortOffset) {
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
            UdpEndpoint mapped = source;
            auto* mappedAddress = reinterpret_cast<sockaddr_in*>(&mapped.addr);
            const int sourcePort = ntohs(mappedAddress->sin_port);
            int mappedPort = sourcePort + mappedPortOffset;
            if (mappedPort > 65535) mappedPort = sourcePort - mappedPortOffset;
            mappedAddress->sin_port = htons(static_cast<uint16_t>(mappedPort));
            const auto response = MakeBindingResponse(buffer.data(), mapped);
            sendto(stunSocket,
                reinterpret_cast<const char*>(response.data()),
                static_cast<int>(response.size()), 0,
                reinterpret_cast<const sockaddr*>(&source.addr), source.addr_len);
        }
    };
    std::thread stunAThread(runStun, stunA, 0);
    std::thread stunBThread(runStun, stunB, 0);
    std::thread stunRandomThread(runStun, stunRandom, 1000);
    std::thread stunRegularThread(runStun, stunRegular, 1);

    Config aConfig;
    aConfig.rendezvous_addr = "127.0.0.1";
    aConfig.rendezvous_port = EndpointPort(rendezvousEndpoint);
    aConfig.room_id = "integration";
    aConfig.peer_id = "a";
    aConfig.target_peer_id = "b";
    aConfig.punch_timeout = 8;
    aConfig.nat_punch_profile = NatPunchProfile::Aggressive;
    aConfig.stun_servers = {
        {"127.0.0.1", EndpointPort(stunAEndpoint)},
        {"127.0.0.2", EndpointPort(stunBEndpoint)},
    };
    Config bConfig = aConfig;
    bConfig.peer_id = "b";
    bConfig.target_peer_id = "a";
    bConfig.stun_servers[1] = {
        "127.0.0.3", EndpointPort(stunRandomEndpoint)};

    Config waitingConfig = bConfig;
    waitingConfig.room_id = "tun-ip-wait";
    waitingConfig.peer_id = "waiting-peer";
    waitingConfig.target_peer_id.clear();
    waitingConfig.local_tun_ipv4 = "10.66.0.99";
    socket_t waitingSocket = kInvalidSocket;
    UdpEndpoint waitingServer{};
    std::string waitingError;
    const bool waitingSocketOpened = OpenRendezvousSocket(
        waitingConfig, 100, &waitingSocket, &waitingServer, &waitingError);
    Expect(waitingSocketOpened,
           "waiting client opens its rendezvous control socket");
    if (waitingSocketOpened) {
        std::atomic<bool> waitingRunning{true};
        UdpEndpoint waitingPeer{};
        std::string waitingPeerId;
        std::vector<TraversalMode> waitingModes;
        NatPunchSession waitingSession;
        bool waitingSelected = false;
        std::thread waitingThread([&] {
            waitingSelected = SelectPeer(
                waitingSocket, waitingConfig, waitingServer, waitingRunning,
                &waitingPeer, &waitingPeerId, &waitingModes, &waitingSession,
                &waitingError);
        });
        const bool tunIpVisible = WaitForListedTunIp(
            waitingConfig, waitingConfig.local_tun_ipv4);
        Expect(tunIpVisible,
               "waiting client reports configured TUN IP before peer selection");
        waitingRunning.store(false);
        waitingThread.join();
        Expect(!waitingSelected,
               "TUN IP reporting does not require a completed connection");
        UnregisterRendezvous(waitingSocket, waitingConfig, waitingServer);
        CloseSocket(waitingSocket);
    }

    socket_t aSocket = kInvalidSocket;
    socket_t bSocket = kInvalidSocket;
    socket_t aPunchSocket = kInvalidSocket;
    socket_t bPunchSocket = kInvalidSocket;
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
    const bool complementaryRoles =
        (aSession.role == NatPunchRole::Initiator
            && bSession.role == NatPunchRole::Responder)
        || (aSession.role == NatPunchRole::Responder
            && bSession.role == NatPunchRole::Initiator);
    Expect(aSession.sessionId == bSession.sessionId
               && aSession.attemptId == bSession.attemptId
               && aSession.punchToken == bSession.punchToken
               && complementaryRoles,
           "paired clients receive one complementary NAT session");

    const uint64_t initialAttemptId = aSession.attemptId;
    const std::string initialPunchToken = aSession.punchToken;
    bool aRetrySynchronized = false;
    bool bRetrySynchronized = false;
    if (aSelected && bSelected) {
        std::thread aRetryThread([&] {
            aRetrySynchronized = RequestNextNatPunchAttempt(
                aSocket, aConfig, aServer, clientsRunning, aPeerId,
                &aSession, &aError);
        });
        std::thread bRetryThread([&] {
            bRetrySynchronized = RequestNextNatPunchAttempt(
                bSocket, bConfig, bServer, clientsRunning, bPeerId,
                &bSession, &bError);
        });
        aRetryThread.join();
        bRetryThread.join();
    }
    Expect(aRetrySynchronized && bRetrySynchronized
               && aSession.sessionId == bSession.sessionId
               && aSession.attemptId == bSession.attemptId
               && aSession.attemptId > initialAttemptId
               && aSession.punchToken == bSession.punchToken
               && aSession.punchToken != initialPunchToken,
           "both clients synchronize a fresh attempt ID and punch token");

    bool aPunched = false;
    bool bPunched = false;
    NatPunchAttemptResult aAttempt;
    NatPunchAttemptResult bAttempt;
    if (aSelected && bSelected) {
        natArmedDropsRemaining.store(2);
        std::thread aPunchThread([&] {
            aPunched = PunchAdaptiveNat(aSocket, &aPunchSocket,
                aConfig, aServer,
                clientsRunning, aPeerId, aSession, &aPeer, &aError,
                &aAttempt, 3);
        });
        std::thread bPunchThread([&] {
            bPunched = PunchAdaptiveNat(bSocket, &bPunchSocket,
                bConfig, bServer,
                clientsRunning, bPeerId, bSession, &bPeer, &bError,
                &bAttempt, 3);
        });
        aPunchThread.join();
        bPunchThread.join();
    }
    Expect(aPunched && bPunched,
           "two clients complete STUN, barrier and PUNCH");
    Expect(aPunchSocket != kInvalidSocket && bPunchSocket != kInvalidSocket
               && aPunchSocket != aSocket && bPunchSocket != bSocket,
           "punch sockets remain separate from rendezvous control sockets");
    Expect(natArmedDropsRemaining.load() == 0,
           "clients retransmit NAT_ARMED after initial packet loss");
    if (!aPunched) std::cerr << "A error: " << aError << '\n';
    if (!bPunched) std::cerr << "B error: " << bError << '\n';
    Expect(aPeer.family == AF_INET && bPeer.family == AF_INET,
           "winner sockets retain confirmed IPv4 peer endpoints");
    Expect(aAttempt.outcome == NatPunchAttemptOutcome::Success
               && bAttempt.outcome == NatPunchAttemptOutcome::Success
               && aAttempt.sessionId == aSession.sessionId
               && bAttempt.attemptId == bSession.attemptId
               && aAttempt.attemptNumber == 3
               && bAttempt.attemptNumber == 3,
           "attempt results correlate success with session and attempt IDs");
    Expect(aAttempt.localBehavior == NatMappingBehavior::EndpointIndependent
               && aAttempt.peerBehavior == NatMappingBehavior::PortDependentRandom
               && aAttempt.plan == "random-sender"
               && aAttempt.targetCount == 1000
               && bAttempt.plan == "random-receiver"
               && bAttempt.socketCount == 256
               && aAttempt.executionRole == "sender"
               && aAttempt.prePunchTtl == 0
               && aAttempt.senderDelayMs == 1000
               && (aAttempt.barrierArmedAcknowledged
                   || bAttempt.barrierArmedAcknowledged)
               && aAttempt.barrierElapsedMs < 3000
               && bAttempt.barrierElapsedMs < 3000
               && bAttempt.executionRole == "receiver"
               && bAttempt.prePunchTtl == 4
               && bAttempt.senderDelayMs == 0
               && bAttempt.prePunchDatagrams == 256
               && aAttempt.profile == NatPunchProfile::Aggressive
               && aAttempt.waveIntervalMs == 5
               && aAttempt.datagramsSent > 0
               && aAttempt.confirmedPeer.family == AF_INET,
           "successful attempt summary retains mapping, plan and endpoint");
    const std::string attemptSummary = FormatNatPunchAttemptResult(aAttempt);
    Expect(attemptSummary.find("outcome=success") != std::string::npos
               && attemptSummary.find("plan=random-sender") != std::string::npos
               && attemptSummary.find("execution_role=sender")
                    != std::string::npos
               && attemptSummary.find("sender_delay_ms=1000")
                    != std::string::npos
               && attemptSummary.find("barrier_armed_ack=")
                    != std::string::npos
               && attemptSummary.find("barrier_elapsed_ms=")
                    != std::string::npos
               && attemptSummary.find("attempt_id="
                   + std::to_string(aSession.attemptId)) != std::string::npos,
           "formatted attempt summary exposes correlated result fields");
    Expect(attemptSummary.find(", attempt=") == std::string::npos,
           "attempt summary does not reuse attempt for two identifiers");

    NatPunchAttemptResult failureSummary;
    failureSummary.outcome = NatPunchAttemptOutcome::BarrierTimeout;
    failureSummary.barrierArmedAcknowledged = true;
    failureSummary.barrierElapsedMs = 8000;
    failureSummary.detail = "barrier timed out\nfalling back";
    const std::string formattedFailure =
        FormatNatPunchAttemptResult(failureSummary);
    Expect(formattedFailure.find("outcome=barrier-timeout")
               != std::string::npos
               && formattedFailure.find("barrier_armed_ack=true")
                    != std::string::npos
               && formattedFailure.find("barrier_elapsed_ms=8000")
                    != std::string::npos
               && formattedFailure.find('\n') == std::string::npos,
           "failure summary has a stable category and remains one line");
    Expect(std::string(NatPunchAttemptOutcomeName(
                      NatPunchAttemptOutcome::StunTimeout)) == "stun-timeout"
               && std::string(NatPunchAttemptOutcomeName(
                      NatPunchAttemptOutcome::StrategyUnsupported))
                      == "strategy-unsupported"
               && std::string(NatPunchAttemptOutcomeName(
                      NatPunchAttemptOutcome::PunchTimeout)) == "punch-timeout",
           "diagnostic outcome names stay stable for log analysis");
    Expect(IsRetryableNatPunchOutcome(NatPunchAttemptOutcome::StunTimeout)
               && IsRetryableNatPunchOutcome(
                   NatPunchAttemptOutcome::BarrierTimeout)
               && IsRetryableNatPunchOutcome(
                   NatPunchAttemptOutcome::PunchTimeout)
               && !IsRetryableNatPunchOutcome(
                   NatPunchAttemptOutcome::StrategyUnsupported),
           "only transient NAT attempt outcomes schedule another attempt");

    const InterruptedPunchResult cancelled = RunInterruptedRandomReceiver(
        aConfig, bConfig, rendezvousEndpoint,
        InterruptedPunchMode::CancelAtBarrier, 1);
    Expect(cancelled.setup && cancelled.peerInfoReceived
               && !cancelled.punched
               && cancelled.attempt.outcome
                      == NatPunchAttemptOutcome::Stopped
               && cancelled.attempt.plan == "random-receiver"
               && cancelled.attempt.socketCount == 256,
           "cancellation closes a maximum random receiver pool at the barrier");
    if (cancelled.attempt.outcome != NatPunchAttemptOutcome::Stopped) {
        std::cerr << "Cancellation error: " << cancelled.error << '\n';
    }

    const InterruptedPunchResult timedOut = RunInterruptedRandomReceiver(
        aConfig, bConfig, rendezvousEndpoint,
        InterruptedPunchMode::PunchTimeout, 2);
    Expect(timedOut.setup && timedOut.peerInfoReceived
               && !timedOut.punched
               && timedOut.attempt.outcome
                      == NatPunchAttemptOutcome::PunchTimeout
               && timedOut.attempt.plan == "random-receiver"
               && timedOut.attempt.socketCount == 256
               && timedOut.attempt.datagramsSent > 0,
           "PUNCH timeout closes a maximum random receiver pool");
    if (timedOut.attempt.outcome != NatPunchAttemptOutcome::PunchTimeout) {
        std::cerr << "Timeout error: " << timedOut.error << '\n';
    }

    const InterruptedPunchResult barrierTimedOut =
        RunInterruptedRandomReceiver(
            aConfig, bConfig, rendezvousEndpoint,
            InterruptedPunchMode::BarrierTimeout, 3);
    Expect(barrierTimedOut.setup && barrierTimedOut.peerInfoReceived
               && !barrierTimedOut.punched
               && barrierTimedOut.attempt.outcome
                      == NatPunchAttemptOutcome::BarrierTimeout
               && barrierTimedOut.attempt.barrierArmedAcknowledged
               && barrierTimedOut.attempt.barrierElapsedMs >= 7500
               && barrierTimedOut.attempt.barrierElapsedMs < 12000
               && barrierTimedOut.attempt.elapsedMs < 15000
               && barrierTimedOut.attempt.datagramsSent == 0
               && barrierTimedOut.error.find("peer did not become ready")
                      != std::string::npos,
           "barrier timeout is capped and reports acknowledged local state");
    if (barrierTimedOut.attempt.outcome
        != NatPunchAttemptOutcome::BarrierTimeout) {
        std::cerr << "Barrier timeout error: "
                  << barrierTimedOut.error << '\n';
    }

    Config regularConfig = aConfig;
    regularConfig.room_id = "mixed-integration";
    regularConfig.peer_id = "regular";
    regularConfig.target_peer_id = "random";
    regularConfig.stun_servers[1] = {
        "127.0.0.4", EndpointPort(stunRegularEndpoint)};
    Config randomConfig = bConfig;
    randomConfig.room_id = regularConfig.room_id;
    randomConfig.peer_id = "random";
    randomConfig.target_peer_id = "regular";
    const PairedPunchResult mixed = RunPairedPunch(
        regularConfig, randomConfig, 2);
    Expect(mixed.setup && mixed.selected
               && mixed.firstPunched && mixed.secondPunched,
           "regular/random peers complete mixed random-range punching");
    if (!mixed.firstPunched) {
        std::cerr << "Mixed regular error: " << mixed.firstError << '\n';
    }
    if (!mixed.secondPunched) {
        std::cerr << "Mixed random error: " << mixed.secondError << '\n';
    }
    Expect(mixed.firstAttempt.localBehavior
                  == NatMappingBehavior::PortDependentRegular
               && mixed.firstAttempt.peerBehavior
                  == NatMappingBehavior::PortDependentRandom
               && mixed.firstAttempt.plan == "mixed-random-sender"
               && mixed.firstAttempt.targetCount == 1000
               && mixed.firstAttempt.socketCount == 1
               && mixed.firstAttempt.executionRole == "sender"
               && mixed.firstAttempt.prePunchTtl == 0
               && mixed.firstAttempt.senderDelayMs == 1000
               && mixed.secondAttempt.localBehavior
                  == NatMappingBehavior::PortDependentRandom
               && mixed.secondAttempt.peerBehavior
                  == NatMappingBehavior::PortDependentRegular
               && mixed.secondAttempt.plan == "mixed-random-receiver"
               && mixed.secondAttempt.targetCount == 17
               && mixed.secondAttempt.socketCount == 128
               && mixed.secondAttempt.portSpan == 8
               && mixed.secondAttempt.executionRole == "receiver"
               && mixed.secondAttempt.prePunchTtl == 7
               && mixed.secondAttempt.senderDelayMs == 0
               && mixed.secondAttempt.prePunchDatagrams == 2176
               && mixed.firstAttempt.datagramsSent > 0
               && mixed.secondAttempt.datagramsSent > 0,
           "mixed attempt summaries expose complementary bounded plans");
    if (aPunched) {
        const std::string legacyPunch = MakeControlMessage(
            "PUNCH", {aConfig.room_id, aPeerId});
        const PeerControlResult legacyResult = HandlePeerControl(
            aPunchSocket, aConfig, aPeer, aPeer, aPeerId, aSession,
            reinterpret_cast<const uint8_t*>(legacyPunch.data()),
            legacyPunch.size());
        Expect(legacyResult.handled && !legacyResult.peerSeen,
               "legacy room/peer PUNCH format is rejected");

        const std::string currentPunch = MakeControlMessage("PUNCH",
            {aSession.sessionId, std::to_string(aSession.attemptId),
             aPeerId, "delayed-nonce", aSession.punchToken});
        const PeerControlResult currentResult = HandlePeerControl(
            aPunchSocket, aConfig, aPeer, aPeer, aPeerId, aSession,
            reinterpret_cast<const uint8_t*>(currentPunch.data()),
            currentPunch.size());
        Expect(currentResult.handled && currentResult.peerSeen,
               "current session-bound PUNCH remains valid after handoff");
    }

    clientsRunning.store(false);
    UnregisterRendezvous(aSocket, aConfig, aServer);
    UnregisterRendezvous(bSocket, bConfig, bServer);
    Expect(WaitForPeersUnlisted(aConfig, {"a", "b"}),
           "control-socket UNREG removes punched peers from rendezvous");
    CloseSocket(aPunchSocket);
    CloseSocket(bPunchSocket);
    CloseSocket(aSocket);
    CloseSocket(bSocket);
    servicesRunning.store(false);
    rendezvousThread.join();
    stunAThread.join();
    stunBThread.join();
    stunRandomThread.join();
    stunRegularThread.join();
    CloseSocket(rendezvousSocket);
    CloseSocket(stunA);
    CloseSocket(stunB);
    CloseSocket(stunRandom);
    CloseSocket(stunRegular);
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
