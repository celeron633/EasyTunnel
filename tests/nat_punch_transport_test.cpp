#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "nat_punch_transport.h"
#include "nat_punch_handshake.h"
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

std::vector<int> sentTtls;
bool CaptureProbeTtl(socket_t sock, const UdpEndpoint&, const std::string&) {
    int ttl = 0;
    if (!SocketTtl(sock, &ttl)) return false;
    sentTtls.push_back(ttl);
    return true;
}

bool FailProbeSend(socket_t, const UdpEndpoint&, const std::string&) {
    return false;
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

    // Reproduce X's queued pre-punch racing a successful probe to Y.
    NatPunchHandshake senderHandshake(NatPunchExecutionRole::Sender, "sender-nonce");
    NatPunchHandshake receiverHandshake(NatPunchExecutionRole::Receiver, "receiver-nonce");
    UdpEndpoint loserEndpoint{};
    ParseUdpEndpoint("127.0.0.1", 12345, &loserEndpoint);
    Expect(senderHandshake.Handle(sender, loserEndpoint, "PUNCH", "receiver-nonce")
               == NatPunchReply::None && !senderHandshake.complete(),
           "sender ignores queued receiver probes instead of selecting a loser");
    Expect(receiverHandshake.Handle(receiver, senderEndpoint, "PUNCH_CONFIRM", "sender-nonce")
               == NatPunchReply::None && !receiverHandshake.complete(),
           "out-of-order confirmation cannot nominate a receiver socket");
    Expect(receiverHandshake.Handle(receiver, senderEndpoint, "PUNCH", "sender-nonce")
               == NatPunchReply::Ack && receiverHandshake.awaitingConfirmation(),
           "receiver nominates the receiving socket but waits for confirmation");
    Expect(receiverHandshake.Handle(sender, senderEndpoint, "PUNCH", "sender-nonce")
               == NatPunchReply::None
               && receiverHandshake.Handle(receiver, loserEndpoint, "PUNCH", "sender-nonce")
                   == NatPunchReply::None,
           "another socket or endpoint cannot replace the nominated path");
    Expect(senderHandshake.Handle(sender, receiverEndpoint, "PUNCH_ACK", "old-nonce")
               == NatPunchReply::None && !senderHandshake.complete(),
           "ACK from an old probe cannot complete the sender");
    // Drop the first ACK; the selected receiver must be able to resend it.
    Expect(receiverHandshake.Handle(receiver, senderEndpoint, "PUNCH", "sender-nonce")
               == NatPunchReply::Ack && !receiverHandshake.complete(),
           "duplicate probe after lost ACK keeps the same receiver path");
    Expect(senderHandshake.Handle(sender, receiverEndpoint, "PUNCH_ACK", "sender-nonce")
               == NatPunchReply::Confirm && senderHandshake.complete()
               && SameUdpEndpoint(senderHandshake.peer(), receiverEndpoint),
           "sender confirms Y despite the earlier probe from X");
    receiverHandshake.Handle(receiver, senderEndpoint, "PUNCH_CONFIRM", "old-nonce");
    Expect(receiverHandshake.awaitingConfirmation(), "wrong confirmation nonce is ignored");
    // Drop CONFIRM, then deliver the receiver's retried ACK.
    Expect(senderHandshake.Handle(sender, receiverEndpoint, "PUNCH_ACK", "sender-nonce")
               == NatPunchReply::Confirm,
           "lost confirmation can be recovered without changing the winner");
    receiverHandshake.Handle(receiver, senderEndpoint, "PUNCH_CONFIRM", "sender-nonce");
    Expect(receiverHandshake.complete() && receiverHandshake.socket() == receiver,
           "matching confirmation commits the nominated receiver socket");
    Expect(senderHandshake.Handle(sender, loserEndpoint, "PUNCH_ACK", "sender-nonce")
               == NatPunchReply::None && SameUdpEndpoint(senderHandshake.peer(), receiverEndpoint),
           "late ACK from another candidate cannot change the committed sender path");

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

    const auto firstProbe = SendNatPunchProbe(sender, receiverEndpoint, "pre-punch",
                                              7, CaptureProbeTtl);
    const auto refreshProbe = SendNatPunchProbe(sender, receiverEndpoint, "refresh",
                                                7, CaptureProbeTtl);
    const auto normalReply = SendNatPunchProbe(sender, receiverEndpoint, "ack",
                                               0, CaptureProbeTtl);
    Expect(firstProbe.sent && refreshProbe.sent && normalReply.sent
               && sentTtls == std::vector<int>({7, 7, originalTtl})
               && SocketTtl(sender, &restoredTtl) && restoredTtl == originalTtl,
           "pre-punch and refresh stay low TTL while confirmation uses original TTL");
    const auto failedProbe = SendNatPunchProbe(sender, receiverEndpoint, "failed",
                                               4, FailProbeSend);
    Expect(!failedProbe.sent && failedProbe.ttlApplied && failedProbe.ttlRestored
               && SocketTtl(sender, &restoredTtl) && restoredTtl == originalTtl,
           "failed probe send restores TTL before subsequent confirmation");
    const auto capturedCount = sentTtls.size();
    const auto unavailableProbe = SendNatPunchProbe(kInvalidSocket, receiverEndpoint,
                                                   "skip", 7, CaptureProbeTtl);
    Expect(!unavailableProbe.sent && sentTtls.size() == capturedCount,
           "TTL application failure never emits an ordinary-TTL probe");

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

    const std::string retriedAck = MakeControlMessage("PUNCH_ACK",
        {punchSession.sessionId, std::to_string(punchSession.attemptId),
         "receiver", "sender-nonce", punchSession.punchToken});
    const auto ackResult = HandlePeerControl(sender, senderConfig,
        receiverEndpoint, receiverEndpoint, "receiver", punchSession,
        reinterpret_cast<const uint8_t*>(retriedAck.data()), retriedAck.size());
    std::vector<uint8_t> confirmBuffer(2048);
    const int confirmLength = recvfrom(receiver,
        reinterpret_cast<char*>(confirmBuffer.data()),
        static_cast<int>(confirmBuffer.size()), 0, nullptr, nullptr);
    std::string confirmType;
    std::vector<std::string> confirmFields;
    Expect(ackResult.peerSeen && confirmLength > 0
               && ParseControlMessage(confirmBuffer.data(), confirmLength,
                                      &confirmType, &confirmFields)
               && confirmType == "PUNCH_CONFIRM" && confirmFields.size() == 5
               && confirmFields[3] == "sender-nonce",
           "data plane retransmits a lost confirmation on the selected path");
    const auto wrongAck = HandlePeerControl(sender, senderConfig,
        receiverEndpoint, loserEndpoint, "receiver", punchSession,
        reinterpret_cast<const uint8_t*>(retriedAck.data()), retriedAck.size());
    Expect(!wrongAck.peerSeen, "data plane rejects ACK from a losing candidate");

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
