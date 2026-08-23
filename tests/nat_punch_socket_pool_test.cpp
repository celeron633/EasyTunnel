#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "nat_punch_socket_pool.h"

namespace {
int failures = 0;
size_t openerCalls = 0;
size_t failAfter = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}

socket_t OpenBoundSocket(int receiveTimeoutMs, int* socketError) {
    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSocket) {
        if (socketError != nullptr) *socketError = GetSocketError();
        return sock;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    if (bind(sock, reinterpret_cast<const sockaddr*>(&local),
             static_cast<socket_len_t>(sizeof(local))) != 0) {
        if (socketError != nullptr) *socketError = GetSocketError();
        CloseSocket(sock);
        return kInvalidSocket;
    }
    SetSocketRecvTimeoutMs(sock, receiveTimeoutMs);
    return sock;
}

socket_t FailingOpener(int receiveTimeoutMs, int* socketError) {
    if (openerCalls++ >= failAfter) {
        if (socketError != nullptr) *socketError = 12345;
        return kInvalidSocket;
    }
    return OpenBoundSocket(receiveTimeoutMs, socketError);
}

UdpEndpoint SocketEndpoint(socket_t sock) {
    UdpEndpoint endpoint{};
    endpoint.addr_len = static_cast<socket_len_t>(sizeof(endpoint.addr));
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&endpoint.addr),
                    &endpoint.addr_len) == 0) {
        endpoint.family = endpoint.addr.ss_family;
    }
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

    for (int round = 0; round < 12; ++round) {
        int socketError = 0;
        socket_t primary = OpenBoundSocket(10, &socketError);
        Expect(primary != kInvalidSocket, "pressure-test primary socket opens");
        if (primary == kInvalidSocket) break;
        NatPunchSocketPool pool(primary, 10, OpenBoundSocket);
        Expect(pool.GrowTo(256, &socketError) && pool.size() == 256,
               "maximum random receiver pool repeatedly opens");
        std::vector<uint8_t> buffer(64);
        socket_t receivingSocket = kInvalidSocket;
        UdpEndpoint source{};
        int received = -1;
        std::string error;
        Expect(pool.Receive(1, &buffer, &receivingSocket,
                            &source, &received, &error)
                   && received < 0,
               "idle socket pool times out without failing");
    }

    int socketError = 0;
    socket_t primary = OpenBoundSocket(20, &socketError);
    NatPunchSocketPool winnerPool(primary, 20, OpenBoundSocket);
    Expect(winnerPool.GrowTo(4, &socketError),
           "winner-selection pool opens");
    socket_t sender = OpenBoundSocket(20, &socketError);
    const socket_t expectedWinner = winnerPool.sockets()[2];
    const UdpEndpoint winnerEndpoint = SocketEndpoint(expectedWinner);
    const uint8_t payload[]{1, 2, 3, 4};
    sendto(sender, reinterpret_cast<const char*>(payload), sizeof(payload), 0,
           reinterpret_cast<const sockaddr*>(&winnerEndpoint.addr),
           winnerEndpoint.addr_len);
    std::vector<uint8_t> buffer(64);
    socket_t receivingSocket = kInvalidSocket;
    UdpEndpoint source{};
    int received = -1;
    std::string error;
    Expect(winnerPool.Receive(500, &buffer, &receivingSocket,
                             &source, &received, &error)
               && received == static_cast<int>(sizeof(payload))
               && receivingSocket == expectedWinner,
           "poll selects the socket that received the datagram");
    socket_t released = winnerPool.ReleaseWinner(receivingSocket);
    Expect(released == expectedWinner && winnerPool.size() == 0,
           "winner ownership transfers out of the pool");
    CloseSocket(released);
    CloseSocket(sender);

    openerCalls = 0;
    failAfter = 3;
    primary = OpenBoundSocket(20, &socketError);
    {
        NatPunchSocketPool limitedPool(primary, 20, FailingOpener);
        Expect(!limitedPool.GrowTo(8, &socketError)
                   && limitedPool.size() == 4
                   && socketError == 12345,
               "resource exhaustion preserves the usable partial pool");
    }
    primary = OpenBoundSocket(20, &socketError);
    Expect(primary != kInvalidSocket,
           "sockets remain available after exhausted pool cleanup");
    CloseSocket(primary);

#ifdef _WIN32
    WSACleanup();
#endif
    if (failures != 0) {
        std::cerr << failures << " NAT punch socket pool test(s) failed\n";
        return 1;
    }
    std::cout << "NAT punch socket pool tests passed\n";
    return 0;
}
