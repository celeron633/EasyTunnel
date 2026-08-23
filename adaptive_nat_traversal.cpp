#include "adaptive_nat_traversal.h"

#include <chrono>
#include <vector>

#include "log.h"
#include "nat_protocol.h"
#include "nat_punch_plan.h"
#include "secure_random.h"
#include "stun_client.h"

namespace {
constexpr int kReceiveTimeoutMs = 100;
constexpr int kStunTimeoutMs = 800;
constexpr int kStunAttempts = 3;
constexpr auto kControlRetryInterval = std::chrono::milliseconds(500);
constexpr auto kPunchInterval = std::chrono::milliseconds(200);

bool Send(socket_t sock, const UdpEndpoint& endpoint,
          const std::string& data) {
    return sendto(sock, data.data(), static_cast<int>(data.size()), 0,
        reinterpret_cast<const sockaddr*>(&endpoint.addr), endpoint.addr_len)
        == static_cast<int>(data.size());
}

UdpEndpoint FromSockaddr(const sockaddr_storage& address, socket_len_t len) {
    UdpEndpoint endpoint{};
    endpoint.addr = address;
    endpoint.addr_len = len;
    endpoint.family = address.ss_family;
    return endpoint;
}

bool SameIpv4Address(const UdpEndpoint& first, const UdpEndpoint& second) {
    if (first.family != AF_INET || second.family != AF_INET) return false;
    const auto* firstAddress =
        reinterpret_cast<const sockaddr_in*>(&first.addr);
    const auto* secondAddress =
        reinterpret_cast<const sockaddr_in*>(&second.addr);
    return firstAddress->sin_addr.s_addr == secondAddress->sin_addr.s_addr;
}

bool MatchesSession(const RendezvousEvent& event,
                    const NatPunchSession& session) {
    return event.sessionId == session.sessionId
        && event.attemptId == session.attemptId;
}

bool ReceiveDatagram(socket_t sock, std::vector<uint8_t>* buffer,
                     UdpEndpoint* source, int* received,
                     std::string* error) {
    sockaddr_storage sourceAddress{};
    socket_len_t sourceLen = static_cast<socket_len_t>(sizeof(sourceAddress));
    *received = recvfrom(sock, reinterpret_cast<char*>(buffer->data()),
        static_cast<int>(buffer->size()), 0,
        reinterpret_cast<sockaddr*>(&sourceAddress), &sourceLen);
    if (*received >= 0) {
        *source = FromSockaddr(sourceAddress, sourceLen);
        return true;
    }
    const int socketError = GetSocketError();
    if (IsRecvTimeout(socketError)
        || IsUdpDestinationUnreachable(socketError)) {
        return true;
    }
    *error = "UDP receive failed during adaptive NAT traversal. err="
        + std::to_string(socketError);
    return false;
}
}  // namespace

bool PunchAdaptiveNat(socket_t* sock, const Config& cfg,
                      const UdpEndpoint& server,
                      const std::atomic<bool>& running,
                      const std::string& matchedPeerId,
                      const NatPunchSession& session,
                      UdpEndpoint* peer, std::string* error) {
    if (sock == nullptr || peer == nullptr || matchedPeerId.empty()
        || session.sessionId.empty() || session.attemptId == 0
        || session.protocolVersion != 2 || session.punchToken.empty()) {
        *error = "Adaptive NAT traversal has no valid rendezvous session";
        return false;
    }
    if (cfg.stun_servers.size() != 2) {
        *error = "Adaptive NAT traversal requires exactly two STUN servers";
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(cfg.punch_timeout);
    socket_t punchSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (punchSocket == kInvalidSocket) {
        *error = "Cannot create the NAT punch socket. err="
            + std::to_string(GetSocketError());
        return false;
    }
    SetSocketRecvTimeoutMs(punchSocket, kReceiveTimeoutMs);

    std::vector<StunProbeResult> probes;
    if (!ProbeStunServers(punchSocket, cfg.stun_servers, kStunTimeoutMs,
                          kStunAttempts, &probes, error)) {
        CloseSocket(punchSocket);
        return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        *error = "Adaptive NAT traversal timed out during STUN discovery";
        CloseSocket(punchSocket);
        return false;
    }
    SetSocketRecvTimeoutMs(punchSocket, kReceiveTimeoutMs);

    const NatMappingAnalysis localAnalysis = ClassifyNatMapping(
        probes[0].mappedEndpoint, probes[1].mappedEndpoint);
    NatPunchObservation localObservation{
        localAnalysis.behavior,
        probes[0].mappedEndpoint,
        probes[1].mappedEndpoint,
    };
    Log(LogLevel::Info, "STUN mapping classified as "
        + std::string(NatMappingBehaviorName(localAnalysis.behavior))
        + "; A=" + FormatUdpEndpoint(probes[0].mappedEndpoint)
        + ", B=" + FormatUdpEndpoint(probes[1].mappedEndpoint)
        + ", delta=" + std::to_string(localAnalysis.portDelta));

    RendezvousClient rendezvous(cfg, server);
    std::vector<uint8_t> buffer(2048);
    RendezvousEvent peerInfoEvent;
    auto nextControlSend = std::chrono::steady_clock::time_point{};
    while (running.load() && std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextControlSend) {
            if (!rendezvous.SendNatInfo(
                    punchSocket, matchedPeerId, session.sessionId,
                    session.attemptId,
                    NatMappingBehaviorName(localAnalysis.behavior),
                    probes[0].mappedEndpoint, probes[1].mappedEndpoint, "-")) {
                Log(LogLevel::Warn, "Failed to send NAT_INFO. err="
                    + std::to_string(GetSocketError()));
            }
            nextControlSend = now + kControlRetryInterval;
        }

        UdpEndpoint source{};
        int received = -1;
        if (!ReceiveDatagram(punchSocket, &buffer, &source, &received, error)) {
            CloseSocket(punchSocket);
            return false;
        }
        if (received < 0) continue;
        const RendezvousEvent event = rendezvous.HandlePacket(
            source, buffer.data(), static_cast<size_t>(received));
        if (event.type == RendezvousEventType::Error) {
            *error = event.error;
            CloseSocket(punchSocket);
            return false;
        }
        if (event.type == RendezvousEventType::NatPeerInfo
            && MatchesSession(event, session)
            && event.peerId == matchedPeerId
            && event.natPunchRole == session.role
            && event.natPunchVersion == session.protocolVersion) {
            peerInfoEvent = event;
            break;
        }
    }
    if (peerInfoEvent.type != RendezvousEventType::NatPeerInfo) {
        *error = running.load() ? "Timed out waiting for peer NAT information"
                                : "Adaptive NAT traversal stopped";
        CloseSocket(punchSocket);
        return false;
    }

    NatMappingBehavior peerBehavior = NatMappingBehavior::Unknown;
    if (!ParseNatMappingBehavior(peerInfoEvent.natPeerInfo.mappingBehavior,
                                 &peerBehavior)) {
        *error = "Rendezvous returned an invalid peer NAT behavior";
        CloseSocket(punchSocket);
        return false;
    }
    const NatPunchObservation peerObservation{
        peerBehavior,
        peerInfoEvent.natPeerInfo.mappedA,
        peerInfoEvent.natPeerInfo.mappedB,
    };
    NatPunchPlan plan;
    if (!BuildNatPunchPlan(localObservation, peerObservation, &plan, error)) {
        CloseSocket(punchSocket);
        return false;
    }
    Log(LogLevel::Info, "NAT punch plan="
        + std::string(NatPunchPlanModeName(plan.mode))
        + ", targets=" + std::to_string(plan.targets.size())
        + ", predicted_port=" + std::to_string(plan.predictedPort)
        + ", span=" + std::to_string(plan.portSpan));

    nextControlSend = std::chrono::steady_clock::time_point{};
    bool startReceived = false;
    while (running.load() && std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextControlSend) {
            if (!rendezvous.SendNatArmed(
                    punchSocket, matchedPeerId, session.sessionId,
                    session.attemptId)) {
                Log(LogLevel::Warn, "Failed to send NAT_ARMED. err="
                    + std::to_string(GetSocketError()));
            }
            nextControlSend = now + kControlRetryInterval;
        }

        UdpEndpoint source{};
        int received = -1;
        if (!ReceiveDatagram(punchSocket, &buffer, &source, &received, error)) {
            CloseSocket(punchSocket);
            return false;
        }
        if (received < 0) continue;
        const RendezvousEvent event = rendezvous.HandlePacket(
            source, buffer.data(), static_cast<size_t>(received));
        if (event.type == RendezvousEventType::Error) {
            *error = event.error;
            CloseSocket(punchSocket);
            return false;
        }
        if (event.type == RendezvousEventType::NatStart
            && MatchesSession(event, session)) {
            startReceived = true;
            break;
        }
    }
    if (!startReceived) {
        *error = running.load() ? "Timed out at the NAT synchronization barrier"
                                : "Adaptive NAT traversal stopped";
        CloseSocket(punchSocket);
        return false;
    }

    std::string randomError;
    const std::string nonce = SecureRandomHex(16, &randomError);
    if (nonce.empty()) {
        *error = "Cannot create NAT punch nonce: " + randomError;
        CloseSocket(punchSocket);
        return false;
    }
    const std::string punch = MakeControlMessage("PUNCH",
        {session.sessionId, std::to_string(session.attemptId),
         cfg.peer_id, nonce, session.punchToken});
    auto nextPunch = std::chrono::steady_clock::time_point{};
    while (running.load() && std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextPunch) {
            for (const UdpEndpoint& target : plan.targets) {
                Send(punchSocket, target, punch);
            }
            nextPunch = now + kPunchInterval;
        }

        UdpEndpoint source{};
        int received = -1;
        if (!ReceiveDatagram(punchSocket, &buffer, &source, &received, error)) {
            CloseSocket(punchSocket);
            return false;
        }
        if (received < 0) continue;

        const RendezvousEvent rendezvousEvent = rendezvous.HandlePacket(
            source, buffer.data(), static_cast<size_t>(received));
        if (rendezvousEvent.type == RendezvousEventType::Error) {
            *error = rendezvousEvent.error;
            CloseSocket(punchSocket);
            return false;
        }
        if (rendezvousEvent.type != RendezvousEventType::None) continue;

        std::string type;
        std::vector<std::string> fields;
        if (!ParseControlMessage(buffer.data(), static_cast<size_t>(received),
                                 &type, &fields)
            || fields.size() != 5
            || fields[0] != session.sessionId
            || fields[1] != std::to_string(session.attemptId)
            || fields[2] != matchedPeerId
            || !IsSafeControlField(fields[3])
            || fields[4] != session.punchToken
            || !SameIpv4Address(source, peerObservation.mappedA)
            || (type != "PUNCH" && type != "PUNCH_ACK")) {
            continue;
        }
        if (type == "PUNCH_ACK" && fields[3] != nonce) continue;

        if (type == "PUNCH") {
            const std::string ack = MakeControlMessage("PUNCH_ACK",
                {session.sessionId, std::to_string(session.attemptId),
                 cfg.peer_id, fields[3], session.punchToken});
            for (int repeat = 0; repeat < 5; ++repeat) {
                Send(punchSocket, source, ack);
            }
        }
        *peer = source;
        CloseSocket(*sock);
        *sock = punchSocket;
        Log(LogLevel::Info, "Adaptive NAT traversal confirmed with "
            + FormatUdpEndpoint(*peer));
        return true;
    }

    *error = running.load() ? "Adaptive NAT punch timed out"
                            : "Adaptive NAT traversal stopped";
    CloseSocket(punchSocket);
    return false;
}
