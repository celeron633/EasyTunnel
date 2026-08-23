#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>

#include "nat_punch_transport.h"

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
