#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "nat_punch_transport.h"
#include "nat_protocol.h"
#include "nat_traversal.h"

namespace {
int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}

socket_t OpenBoundSocket(UdpEndpoint* endpoint) {
    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSocket) return sock;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    if (bind(sock, reinterpret_cast<const sockaddr*>(&local),
             static_cast<socket_len_t>(sizeof(local))) != 0) {
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
    SetSocketRecvTimeoutMs(sock, 500);
    return sock;
}

bool SocketTtl(socket_t sock, int* ttl) {
    socket_len_t length = static_cast<socket_len_t>(sizeof(*ttl));
    return getsockopt(sock, IPPROTO_IP, IP_TTL,
                      reinterpret_cast<char*>(ttl), &length) == 0;
}

UdpEndpoint FromSockaddr(const sockaddr_storage& address, socket_len_t len) {
    UdpEndpoint endpoint{};
    endpoint.addr = address;
    endpoint.addr_len = len;
    endpoint.family = address.ss_family;
    return endpoint;
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

    UdpEndpoint senderEndpoint{};
    UdpEndpoint receiverEndpoint{};
    socket_t sender = OpenBoundSocket(&senderEndpoint);
    socket_t receiver = OpenBoundSocket(&receiverEndpoint);
    Expect(sender != kInvalidSocket && receiver != kInvalidSocket,
           "loopback UDP sockets open");

    int originalTtl = 0;
    int socketError = 0;
    Expect(SocketTtl(sender, &originalTtl) && originalTtl > 0,
           "original IPv4 TTL is readable");
    const uint8_t testTtl = originalTtl == 4 ? 7 : 4;
    {
        ScopedIpv4SocketTtl ttlScope(sender, testTtl, &socketError);
        int activeTtl = 0;
        Expect(ttlScope.applied() && socketError == 0
                   && SocketTtl(sender, &activeTtl)
                   && activeTtl == static_cast<int>(testTtl),
               "TTL scope applies the requested low TTL");

        const char payload[] = "ttl-punch";
        const int sent = sendto(sender, payload,
            static_cast<int>(sizeof(payload)), 0,
            reinterpret_cast<const sockaddr*>(&receiverEndpoint.addr),
            receiverEndpoint.addr_len);
        Expect(sent == static_cast<int>(sizeof(payload)),
               "UDP datagram sends while low TTL is active");
        Expect(ttlScope.Restore(&socketError) && socketError == 0,
               "TTL scope restores explicitly");

        char receivedPayload[32]{};
        const int received = recvfrom(receiver, receivedPayload,
            static_cast<int>(sizeof(receivedPayload)), 0, nullptr, nullptr);
        Expect(received == static_cast<int>(sizeof(payload))
                   && std::memcmp(receivedPayload, payload, sizeof(payload)) == 0,
               "low-TTL UDP datagram reaches a loopback receiver");
    }
    int restoredTtl = 0;
    Expect(SocketTtl(sender, &restoredTtl) && restoredTtl == originalTtl,
           "socket TTL remains restored after scope destruction");
    {
        ScopedIpv4SocketTtl ttlScope(sender, 7, &socketError);
        Expect(ttlScope.applied(), "second TTL scope applies");
    }
    Expect(SocketTtl(sender, &restoredTtl) && restoredTtl == originalTtl,
           "TTL scope destructor restores an early-return path");

    ScopedIpv4SocketTtl invalidScope(kInvalidSocket, 7, &socketError);
    Expect(!invalidScope.applied(), "invalid socket cannot acquire a TTL scope");

    std::atomic<bool> running{true};
    Expect(WaitForNatPunchDelay(running, 10),
           "sender delay completes while traversal is running");
    const auto cancelStarted = std::chrono::steady_clock::now();
    std::thread cancelThread([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        running.store(false);
    });
    const bool delayCompleted = WaitForNatPunchDelay(running, 2000);
    cancelThread.join();
    const auto cancelElapsed = std::chrono::duration_cast<
        std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - cancelStarted).count();
    Expect(!delayCompleted && cancelElapsed < 1500,
           "shutdown interrupts sender delay without waiting for its timeout");

    Config senderConfig;
    senderConfig.room_id = "peer-close-room";
    senderConfig.peer_id = "sender";
    Config receiverConfig = senderConfig;
    receiverConfig.peer_id = "receiver";
    NatPunchSession punchSession;
    punchSession.sessionId = "peer-close-session";
    punchSession.attemptId = 42;
    punchSession.role = NatPunchRole::Initiator;
    punchSession.protocolVersion = kNatPunchProtocolVersionNumber;
    punchSession.punchToken =
        "0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef";

    Expect(SendPeerDisconnect(
               sender, senderConfig, receiverEndpoint, punchSession),
           "authenticated peer disconnect is sent over the data socket");
    std::vector<uint8_t> closeBuffer(2048);
    sockaddr_storage closeSourceAddress{};
    socket_len_t closeSourceLength =
        static_cast<socket_len_t>(sizeof(closeSourceAddress));
    const int closeLength = recvfrom(receiver,
        reinterpret_cast<char*>(closeBuffer.data()),
        static_cast<int>(closeBuffer.size()), 0,
        reinterpret_cast<sockaddr*>(&closeSourceAddress), &closeSourceLength);
    const PeerControlResult closeResult = HandlePeerControl(
        receiver, receiverConfig, senderEndpoint,
        FromSockaddr(closeSourceAddress, closeSourceLength), "sender",
        punchSession, closeBuffer.data(),
        closeLength > 0 ? static_cast<size_t>(closeLength) : 0);
    Expect(closeLength > 0 && closeResult.handled && closeResult.peerSeen
               && closeResult.peerDisconnectRequested,
           "current peer disconnect requests immediate tunnel shutdown");

    const std::string forgedClose = MakeControlMessage("PEER_CLOSE",
        {punchSession.sessionId, std::to_string(punchSession.attemptId),
         "sender", "wrong-token"});
    const PeerControlResult forgedResult = HandlePeerControl(
        receiver, receiverConfig, senderEndpoint, senderEndpoint, "sender",
        punchSession,
        reinterpret_cast<const uint8_t*>(forgedClose.data()),
        forgedClose.size());
    Expect(forgedResult.handled && !forgedResult.peerSeen
               && !forgedResult.peerDisconnectRequested,
           "peer disconnect with the wrong token cannot stop the tunnel");

    const std::string validClose = MakeControlMessage("PEER_CLOSE",
        {punchSession.sessionId, std::to_string(punchSession.attemptId),
         "sender", punchSession.punchToken});
    const PeerControlResult wrongSourceResult = HandlePeerControl(
        receiver, receiverConfig, senderEndpoint, receiverEndpoint, "sender",
        punchSession,
        reinterpret_cast<const uint8_t*>(validClose.data()), validClose.size());
    Expect(wrongSourceResult.handled && !wrongSourceResult.peerSeen
               && !wrongSourceResult.peerDisconnectRequested,
           "peer disconnect from an unexpected endpoint is ignored");

    NatPunchSession invalidSession = punchSession;
    invalidSession.protocolVersion = 0;
    Expect(!SendPeerDisconnect(
               sender, senderConfig, receiverEndpoint, invalidSession),
           "invalid sessions cannot emit peer disconnect messages");

    CloseSocket(sender);
    CloseSocket(receiver);
#ifdef _WIN32
    WSACleanup();
#endif
    if (failures != 0) {
        std::cerr << failures << " NAT punch transport test(s) failed\n";
        return 1;
    }
    std::cout << "NAT punch transport tests passed\n";
    return 0;
}
