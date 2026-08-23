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
constexpr auto kAttemptRetryGrace = std::chrono::seconds(5);

const char* NatPunchRoleName(NatPunchRole role) {
    switch (role) {
        case NatPunchRole::Initiator: return "initiator";
        case NatPunchRole::Responder: return "responder";
        default: return "unknown";
    }
}

bool IsStunTimeoutFailure(const std::string& error) {
    return error.find("timeout") != std::string::npos
        || error.find("timed out") != std::string::npos
        || error.find("did not respond") != std::string::npos;
}

std::string SingleLine(std::string value) {
    for (char& character : value) {
        if (character == '\r' || character == '\n' || character == '\t') {
            character = ' ';
        }
    }
    return value;
}

class AttemptResultScope {
public:
    AttemptResultScope(NatPunchAttemptResult* result,
                       NatPunchAttemptResult* output)
        : result_(result), output_(output), startedAt_(
              std::chrono::steady_clock::now()) {}

    ~AttemptResultScope() {
        result_->elapsedMs = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAt_).count();
        if (output_ != nullptr) *output_ = *result_;
        const LogLevel level = result_->outcome == NatPunchAttemptOutcome::Success
            ? LogLevel::Info
            : result_->outcome == NatPunchAttemptOutcome::Stopped
                ? LogLevel::Debug
                : LogLevel::Warn;
        Log(level, FormatNatPunchAttemptResult(*result_));
    }

private:
    NatPunchAttemptResult* result_;
    NatPunchAttemptResult* output_;
    std::chrono::steady_clock::time_point startedAt_;
};

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

const char* NatPunchAttemptOutcomeName(NatPunchAttemptOutcome outcome) {
    switch (outcome) {
        case NatPunchAttemptOutcome::InvalidInput: return "invalid-input";
        case NatPunchAttemptOutcome::SocketError: return "socket-error";
        case NatPunchAttemptOutcome::StunTimeout: return "stun-timeout";
        case NatPunchAttemptOutcome::StunFailure: return "stun-failure";
        case NatPunchAttemptOutcome::PeerInfoTimeout:
            return "peer-info-timeout";
        case NatPunchAttemptOutcome::StrategyUnsupported:
            return "strategy-unsupported";
        case NatPunchAttemptOutcome::BarrierTimeout: return "barrier-timeout";
        case NatPunchAttemptOutcome::PunchTimeout: return "punch-timeout";
        case NatPunchAttemptOutcome::ControlError: return "control-error";
        case NatPunchAttemptOutcome::Stopped: return "stopped";
        case NatPunchAttemptOutcome::Success: return "success";
        default: return "unknown";
    }
}

bool IsRetryableNatPunchOutcome(NatPunchAttemptOutcome outcome) {
    return outcome == NatPunchAttemptOutcome::StunTimeout
        || outcome == NatPunchAttemptOutcome::PeerInfoTimeout
        || outcome == NatPunchAttemptOutcome::BarrierTimeout
        || outcome == NatPunchAttemptOutcome::PunchTimeout;
}

std::string FormatNatPunchAttemptResult(
    const NatPunchAttemptResult& result) {
    const std::string endpoint = result.confirmedPeer.family == AF_UNSPEC
        ? "-" : FormatUdpEndpoint(result.confirmedPeer);
    return "NAT punch attempt summary: session="
        + (result.sessionId.empty() ? "-" : result.sessionId)
        + ", attempt=" + std::to_string(result.attemptId)
        + ", local_peer="
        + (result.localPeerId.empty() ? "-" : result.localPeerId)
        + ", remote_peer="
        + (result.remotePeerId.empty() ? "-" : result.remotePeerId)
        + ", role=" + NatPunchRoleName(result.role)
        + ", outcome=" + NatPunchAttemptOutcomeName(result.outcome)
        + ", local=" + NatMappingBehaviorName(result.localBehavior)
        + ", peer=" + NatMappingBehaviorName(result.peerBehavior)
        + ", plan=" + result.plan
        + ", targets=" + std::to_string(result.targetCount)
        + ", endpoint=" + endpoint
        + ", elapsed_ms=" + std::to_string(result.elapsedMs)
        + ", detail=" + SingleLine(result.detail.empty() ? "-" : result.detail);
}

bool RequestNextNatPunchAttempt(socket_t sock, const Config& cfg,
                                const UdpEndpoint& server,
                                const std::atomic<bool>& running,
                                const std::string& matchedPeerId,
                                NatPunchSession* session,
                                std::string* error) {
    if (sock == kInvalidSocket || session == nullptr
        || matchedPeerId.empty() || session->sessionId.empty()
        || session->attemptId == 0 || session->protocolVersion != 2) {
        if (error != nullptr) *error = "Cannot retry an invalid NAT session";
        return false;
    }

    std::string ignoredError;
    if (error == nullptr) error = &ignoredError;
    const uint64_t previousAttemptId = session->attemptId;
    RendezvousClient rendezvous(cfg, server);
    std::vector<uint8_t> buffer(2048);
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(cfg.punch_timeout) + kAttemptRetryGrace;
    auto nextSend = std::chrono::steady_clock::time_point{};
    SetSocketRecvTimeoutMs(sock, kReceiveTimeoutMs);

    while (running.load() && std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextSend) {
            if (!rendezvous.SendNatRetry(
                    sock, matchedPeerId, session->sessionId,
                    previousAttemptId)) {
                Log(LogLevel::Warn, "Failed to send NAT_RETRY. err="
                    + std::to_string(GetSocketError()));
            }
            nextSend = now + kControlRetryInterval;
        }

        UdpEndpoint source{};
        int received = -1;
        if (!ReceiveDatagram(sock, &buffer, &source, &received, error)) {
            return false;
        }
        if (received < 0) continue;
        const RendezvousEvent event = rendezvous.HandlePacket(
            source, buffer.data(), static_cast<size_t>(received));
        if (event.type == RendezvousEventType::Error) {
            *error = event.error;
            return false;
        }
        if (event.type == RendezvousEventType::NatRetryWait
            && event.sessionId == session->sessionId
            && event.attemptId == previousAttemptId) {
            continue;
        }
        if (event.type != RendezvousEventType::NatAttempt) continue;
        if (event.sessionId != session->sessionId
            || event.attemptId <= previousAttemptId
            || event.natPunchRole != session->role
            || event.natPunchVersion != session->protocolVersion
            || event.natPunchToken.empty()) {
            *error = "Rendezvous returned an invalid NAT retry attempt";
            return false;
        }

        session->attemptId = event.attemptId;
        session->punchToken = event.natPunchToken;
        Log(LogLevel::Info, "NAT punch retry synchronized: session="
            + session->sessionId + ", previous_attempt="
            + std::to_string(previousAttemptId) + ", attempt="
            + std::to_string(session->attemptId));
        return true;
    }

    *error = running.load() ? "Timed out waiting for the next NAT attempt"
                            : "NAT attempt retry stopped";
    return false;
}

bool PunchAdaptiveNat(socket_t* sock, const Config& cfg,
                      const UdpEndpoint& server,
                      const std::atomic<bool>& running,
                      const std::string& matchedPeerId,
                      const NatPunchSession& session,
                      UdpEndpoint* peer, std::string* error,
                      NatPunchAttemptResult* attemptResult) {
    NatPunchAttemptResult attempt;
    attempt.sessionId = session.sessionId;
    attempt.attemptId = session.attemptId;
    attempt.localPeerId = cfg.peer_id;
    attempt.remotePeerId = matchedPeerId;
    attempt.role = session.role;
    attempt.detail = "Adaptive NAT traversal did not start";
    AttemptResultScope attemptScope(&attempt, attemptResult);
    std::string ignoredError;
    if (error == nullptr) error = &ignoredError;
    const auto fail = [&](NatPunchAttemptOutcome outcome,
                          const std::string& detail) {
        attempt.outcome = outcome;
        attempt.detail = detail;
        *error = detail;
        return false;
    };

    if (sock == nullptr || peer == nullptr || matchedPeerId.empty()
        || session.sessionId.empty() || session.attemptId == 0
        || session.protocolVersion != 2 || session.punchToken.empty()) {
        return fail(NatPunchAttemptOutcome::InvalidInput,
                    "Adaptive NAT traversal has no valid rendezvous session");
    }
    if (cfg.stun_servers.size() != 2) {
        return fail(NatPunchAttemptOutcome::InvalidInput,
                    "Adaptive NAT traversal requires exactly two STUN servers");
    }

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(cfg.punch_timeout);
    socket_t punchSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (punchSocket == kInvalidSocket) {
        return fail(NatPunchAttemptOutcome::SocketError,
                    "Cannot create the NAT punch socket. err="
                        + std::to_string(GetSocketError()));
    }
    SetSocketRecvTimeoutMs(punchSocket, kReceiveTimeoutMs);

    std::vector<StunProbeResult> probes;
    if (!ProbeStunServers(punchSocket, cfg.stun_servers, kStunTimeoutMs,
                          kStunAttempts, &probes, error)) {
        const NatPunchAttemptOutcome outcome = IsStunTimeoutFailure(*error)
            ? NatPunchAttemptOutcome::StunTimeout
            : NatPunchAttemptOutcome::StunFailure;
        CloseSocket(punchSocket);
        return fail(outcome, *error);
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        CloseSocket(punchSocket);
        return fail(NatPunchAttemptOutcome::StunTimeout,
                    "Adaptive NAT traversal timed out during STUN discovery");
    }
    SetSocketRecvTimeoutMs(punchSocket, kReceiveTimeoutMs);

    const NatMappingAnalysis localAnalysis = ClassifyNatMapping(
        probes[0].mappedEndpoint, probes[1].mappedEndpoint);
    attempt.localBehavior = localAnalysis.behavior;
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
            return fail(NatPunchAttemptOutcome::SocketError, *error);
        }
        if (received < 0) continue;
        const RendezvousEvent event = rendezvous.HandlePacket(
            source, buffer.data(), static_cast<size_t>(received));
        if (event.type == RendezvousEventType::Error) {
            CloseSocket(punchSocket);
            return fail(NatPunchAttemptOutcome::ControlError, event.error);
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
        const bool stillRunning = running.load();
        CloseSocket(punchSocket);
        return fail(stillRunning
                        ? NatPunchAttemptOutcome::PeerInfoTimeout
                        : NatPunchAttemptOutcome::Stopped,
                    stillRunning ? "Timed out waiting for peer NAT information"
                                 : "Adaptive NAT traversal stopped");
    }

    NatMappingBehavior peerBehavior = NatMappingBehavior::Unknown;
    if (!ParseNatMappingBehavior(peerInfoEvent.natPeerInfo.mappingBehavior,
                                 &peerBehavior)) {
        CloseSocket(punchSocket);
        return fail(NatPunchAttemptOutcome::ControlError,
                    "Rendezvous returned an invalid peer NAT behavior");
    }
    attempt.peerBehavior = peerBehavior;
    const NatPunchObservation peerObservation{
        peerBehavior,
        peerInfoEvent.natPeerInfo.mappedA,
        peerInfoEvent.natPeerInfo.mappedB,
    };
    NatPunchPlan plan;
    if (!BuildNatPunchPlan(localObservation, peerObservation, &plan, error)) {
        CloseSocket(punchSocket);
        return fail(NatPunchAttemptOutcome::StrategyUnsupported, *error);
    }
    attempt.plan = NatPunchPlanModeName(plan.mode);
    attempt.targetCount = plan.targets.size();
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
            return fail(NatPunchAttemptOutcome::SocketError, *error);
        }
        if (received < 0) continue;
        const RendezvousEvent event = rendezvous.HandlePacket(
            source, buffer.data(), static_cast<size_t>(received));
        if (event.type == RendezvousEventType::Error) {
            CloseSocket(punchSocket);
            return fail(NatPunchAttemptOutcome::ControlError, event.error);
        }
        if (event.type == RendezvousEventType::NatStart
            && MatchesSession(event, session)) {
            startReceived = true;
            break;
        }
    }
    if (!startReceived) {
        const bool stillRunning = running.load();
        CloseSocket(punchSocket);
        return fail(stillRunning
                        ? NatPunchAttemptOutcome::BarrierTimeout
                        : NatPunchAttemptOutcome::Stopped,
                    stillRunning
                        ? "Timed out at the NAT synchronization barrier"
                        : "Adaptive NAT traversal stopped");
    }

    std::string randomError;
    const std::string nonce = SecureRandomHex(16, &randomError);
    if (nonce.empty()) {
        CloseSocket(punchSocket);
        return fail(NatPunchAttemptOutcome::ControlError,
                    "Cannot create NAT punch nonce: " + randomError);
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
            return fail(NatPunchAttemptOutcome::SocketError, *error);
        }
        if (received < 0) continue;

        const RendezvousEvent rendezvousEvent = rendezvous.HandlePacket(
            source, buffer.data(), static_cast<size_t>(received));
        if (rendezvousEvent.type == RendezvousEventType::Error) {
            CloseSocket(punchSocket);
            return fail(NatPunchAttemptOutcome::ControlError,
                        rendezvousEvent.error);
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
        attempt.outcome = NatPunchAttemptOutcome::Success;
        attempt.confirmedPeer = source;
        attempt.detail = "Authenticated PUNCH/PUNCH_ACK confirmed";
        Log(LogLevel::Info, "Adaptive NAT traversal confirmed with "
            + FormatUdpEndpoint(*peer));
        return true;
    }

    const bool stillRunning = running.load();
    CloseSocket(punchSocket);
    return fail(stillRunning ? NatPunchAttemptOutcome::PunchTimeout
                             : NatPunchAttemptOutcome::Stopped,
                stillRunning ? "Adaptive NAT punch timed out"
                             : "Adaptive NAT traversal stopped");
}
