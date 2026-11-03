#include "adaptive_nat_traversal.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include "log.h"
#include "nat_protocol.h"
#include "nat_punch_handshake.h"
#include "nat_punch_scheduler.h"
#include "nat_punch_plan.h"
#include "nat_punch_socket_pool.h"
#include "nat_punch_transport.h"
#include "secure_random.h"
#include "stun_client.h"

namespace {
constexpr int kReceiveTimeoutMs = 100;
constexpr int kStunTimeoutMs = 800;
constexpr int kStunAttempts = 3;
constexpr auto kControlRetryInterval = std::chrono::milliseconds(500);
constexpr auto kAttemptRetryGrace = std::chrono::seconds(5);
constexpr auto kBarrierTimeout = std::chrono::seconds(8);

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

void RecordProbe(const NatPunchSendResult& result, size_t targetIndex,
                 NatPunchAttemptResult* attempt, std::vector<bool>* probed) {
    ++attempt->probeAttempts;
    if (!result.sent) {
        ++attempt->probeSendFailures;
        return;
    }
    ++attempt->datagramsSent;
    if (result.ttlApplied) ++attempt->lowTtlDatagrams;
    if (!(*probed)[targetIndex]) {
        (*probed)[targetIndex] = true;
        ++attempt->targetsProbed;
    }
}

bool SendPrePunchWave(const NatPunchSocketPool& sockets,
                      const std::vector<UdpEndpoint>& targets,
                      const std::string& punch, uint8_t ttl,
                      const NatPunchAttemptPolicy& policy,
                      NatPunchScheduler::TimePoint deadline,
                      const std::atomic<bool>& running,
                      NatPunchAttemptResult* attempt, std::vector<bool>* probed,
                      size_t* ttlSocketCount, std::string* error) {
    *ttlSocketCount = 0;
    int firstApplyError = 0;
    std::vector<bool> applied(sockets.size(), false);
    NatPunchScheduler scheduler(sockets.size() * targets.size(),
        policy.scanBatchSize, policy.scanBatchIntervalMs, 0, false);
    while (running.load() && std::chrono::steady_clock::now() < deadline
           && !scheduler.finished() && attempt->probeAttempts < policy.datagramBudget) {
        const auto now = std::chrono::steady_clock::now();
        const auto batch = scheduler.TakeBatch(now,
            policy.datagramBudget - attempt->probeAttempts);
        if (batch.count == 0) {
            WaitForNatPunchDelay(running, static_cast<uint32_t>(
                NatPunchPollTimeoutMs(now, scheduler.nextSend(), deadline, 25)));
            continue;
        }
        for (size_t i = batch.offset; i < batch.offset + batch.count; ++i) {
            if (!running.load() || std::chrono::steady_clock::now() >= deadline) break;
            const size_t socketIndex = i / targets.size();
            const size_t targetIndex = i % targets.size();
            const auto result = SendNatPunchProbe(
                sockets.sockets()[socketIndex], targets[targetIndex], punch, ttl);
            RecordProbe(result, targetIndex, attempt, probed);
            if (result.sent) ++attempt->prePunchDatagrams;
            if (!result.ttlRestored) {
                *error = "Cannot restore NAT punch socket TTL. err="
                    + std::to_string(result.socketError);
                return false;
            }
            if (!result.ttlApplied) {
                if (firstApplyError == 0) firstApplyError = result.socketError;
            } else if (!applied[socketIndex]) {
                applied[socketIndex] = true;
                ++*ttlSocketCount;
            }
        }
    }
    if (!running.load()) return true;
    if (*ttlSocketCount == 0) {
        Log(LogLevel::Warn, "Low-TTL NAT pre-punch is unavailable. err="
            + std::to_string(firstApplyError));
    } else if (*ttlSocketCount < sockets.size()) {
        Log(LogLevel::Warn, "Applied low TTL to "
            + std::to_string(*ttlSocketCount) + "/"
            + std::to_string(sockets.size()) + " NAT punch sockets");
    }
    return true;
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

uint32_t LimitNatPunchBarrierWaitMs(uint64_t remainingAttemptMs) {
    const uint64_t barrierTimeoutMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            kBarrierTimeout).count());
    return static_cast<uint32_t>((std::min)(
        remainingAttemptMs, barrierTimeoutMs));
}

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
        + ", attempt_id=" + std::to_string(result.attemptId)
        + ", attempt_index=" + std::to_string(result.attemptNumber)
        + ", local_peer="
        + (result.localPeerId.empty() ? "-" : result.localPeerId)
        + ", remote_peer="
        + (result.remotePeerId.empty() ? "-" : result.remotePeerId)
        + ", role=" + NatPunchRoleName(result.role)
        + ", profile=" + NatPunchProfileName(result.profile)
        + ", outcome=" + NatPunchAttemptOutcomeName(result.outcome)
        + ", local=" + NatMappingBehaviorName(result.localBehavior)
        + ", peer=" + NatMappingBehaviorName(result.peerBehavior)
        + ", plan=" + result.plan
        + ", targets=" + std::to_string(result.targetCount)
        + ", sockets=" + std::to_string(result.socketCount)
        + ", span=" + std::to_string(result.portSpan)
        + ", execution_role=" + result.executionRole
        + ", pre_punch_ttl=" + std::to_string(result.prePunchTtl)
        + ", sender_delay_ms=" + std::to_string(result.senderDelayMs)
        + ", pre_punch_datagrams="
        + std::to_string(result.prePunchDatagrams)
        + ", low_ttl_datagrams=" + std::to_string(result.lowTtlDatagrams)
        + ", barrier_armed_ack="
        + (result.barrierArmedAcknowledged ? "true" : "false")
        + ", barrier_elapsed_ms="
        + std::to_string(result.barrierElapsedMs)
        + ", wave_interval_ms=" + std::to_string(result.waveIntervalMs)
        + ", datagrams=" + std::to_string(result.datagramsSent)
        + "/" + std::to_string(result.datagramBudget)
        + ", probe_attempts=" + std::to_string(result.probeAttempts)
        + ", probe_send_failures=" + std::to_string(result.probeSendFailures)
        + ", targets_probed=" + std::to_string(result.targetsProbed)
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
        || session->attemptId == 0
        || session->protocolVersion != kNatPunchProtocolVersionNumber) {
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
            + session->sessionId + ", previous_attempt_id="
            + std::to_string(previousAttemptId) + ", attempt_id="
            + std::to_string(session->attemptId));
        return true;
    }

    *error = running.load() ? "Timed out waiting for the next NAT attempt"
                            : "NAT attempt retry stopped";
    return false;
}

bool PunchAdaptiveNat(socket_t controlSocket, socket_t* punchSocketOut,
                      const Config& cfg,
                      const UdpEndpoint& server,
                      const std::atomic<bool>& running,
                      const std::string& matchedPeerId,
                      const NatPunchSession& session,
                      UdpEndpoint* peer, std::string* error,
                      NatPunchAttemptResult* attemptResult,
                      uint16_t attemptNumber) {
    NatPunchAttemptResult attempt;
    attempt.sessionId = session.sessionId;
    attempt.attemptId = session.attemptId;
    attempt.attemptNumber = (std::max)(uint16_t{1}, attemptNumber);
    attempt.localPeerId = cfg.peer_id;
    attempt.remotePeerId = matchedPeerId;
    attempt.role = session.role;
    attempt.profile = cfg.nat_punch_profile;
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

    if (controlSocket == kInvalidSocket || punchSocketOut == nullptr
        || *punchSocketOut != kInvalidSocket || peer == nullptr
        || matchedPeerId.empty()
        || session.sessionId.empty() || session.attemptId == 0
        || session.protocolVersion != kNatPunchProtocolVersionNumber
        || session.punchToken.empty()) {
        return fail(NatPunchAttemptOutcome::InvalidInput,
                    "Adaptive NAT traversal has no valid rendezvous session");
    }
    if (cfg.stun_servers.size() != 2) {
        return fail(NatPunchAttemptOutcome::InvalidInput,
                    "Adaptive NAT traversal requires exactly two STUN servers");
    }

    SetSocketRecvTimeoutMs(controlSocket, kReceiveTimeoutMs);
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
                    controlSocket, matchedPeerId, session.sessionId,
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
        if (!ReceiveDatagram(controlSocket, &buffer, &source, &received, error)) {
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
    const NatPunchAttemptPolicy policy = ResolveNatPunchAttemptPolicy(
        cfg.nat_punch_profile, attempt.attemptNumber);
    NatPunchPlan plan;
    if (!BuildNatPunchPlan(
            localObservation, peerObservation, policy, &plan, error)) {
        CloseSocket(punchSocket);
        return fail(NatPunchAttemptOutcome::StrategyUnsupported, *error);
    }
    if ((plan.mode == NatPunchPlanMode::RandomSender
            || plan.mode == NatPunchPlanMode::MixedRandomSender)
        && !BuildRandomPortTargets(
            peerObservation, policy.randomTargetPortCount,
            &plan.targets, error)) {
        CloseSocket(punchSocket);
        return fail(NatPunchAttemptOutcome::ControlError, *error);
    }

    NatPunchSocketPool punchSockets(punchSocket, kReceiveTimeoutMs);
    if (plan.mode == NatPunchPlanMode::RandomReceiver
        || plan.mode == NatPunchPlanMode::MixedRandomReceiver) {
        int socketError = 0;
        if (!punchSockets.GrowTo(plan.receiverSocketCount, &socketError)) {
            Log(LogLevel::Warn,
                "Stopped growing random receiver socket pool at "
                + std::to_string(punchSockets.size())
                + "/" + std::to_string(plan.receiverSocketCount)
                + ". err=" + std::to_string(socketError));
        }
    }
    attempt.plan = NatPunchPlanModeName(plan.mode);
    attempt.targetCount = plan.targets.size();
    attempt.socketCount = punchSockets.size();
    attempt.portSpan = plan.portSpan;
    attempt.datagramBudget = policy.datagramBudget;
    const NatPunchExecutionPolicy executionPolicy =
        ResolveNatPunchExecutionPolicy(
            plan.mode, session.role == NatPunchRole::Initiator,
            attempt.attemptNumber);
    attempt.executionRole = NatPunchExecutionRoleName(executionPolicy.role);
    attempt.prePunchTtl = executionPolicy.prePunchTtl;
    attempt.senderDelayMs = executionPolicy.senderDelayMs;
    Log(LogLevel::Info, "NAT punch plan="
        + std::string(NatPunchPlanModeName(plan.mode))
        + ", profile=" + NatPunchProfileName(cfg.nat_punch_profile)
        + ", attempt_index=" + std::to_string(attempt.attemptNumber)
        + ", targets=" + std::to_string(plan.targets.size())
        + ", sockets=" + std::to_string(punchSockets.size())
        + ", predicted_port=" + std::to_string(plan.predictedPort)
        + ", span=" + std::to_string(plan.portSpan)
        + ", range_scale=" + std::to_string(policy.rangeScale)
        + ", execution_role=" + attempt.executionRole
        + ", pre_punch_ttl=" + std::to_string(attempt.prePunchTtl)
        + ", sender_delay_ms=" + std::to_string(attempt.senderDelayMs)
        + ", datagram_budget=" + std::to_string(policy.datagramBudget));

    std::string randomError;
    const std::string nonce = SecureRandomHex(16, &randomError);
    if (nonce.empty()) {
        return fail(NatPunchAttemptOutcome::ControlError,
                    "Cannot create NAT punch nonce: " + randomError);
    }
    const std::string punch = MakeControlMessage("PUNCH",
        {session.sessionId, std::to_string(session.attemptId),
         cfg.peer_id, nonce, session.punchToken});

    bool lowTtlReceiver = false;
    std::vector<bool> probedTargets(plan.targets.size(), false);
    if (executionPolicy.role == NatPunchExecutionRole::Receiver
        && executionPolicy.prePunchTtl > 0) {
        size_t ttlSocketCount = 0;
        if (!SendPrePunchWave(punchSockets, plan.targets, punch,
                executionPolicy.prePunchTtl, policy, deadline, running,
                &attempt, &probedTargets, &ttlSocketCount, error)) {
            return fail(NatPunchAttemptOutcome::SocketError, *error);
        }
        lowTtlReceiver = ttlSocketCount != 0;
        Log(LogLevel::Info, "NAT pre-punch ttl="
            + std::to_string(executionPolicy.prePunchTtl)
            + ", sockets=" + std::to_string(ttlSocketCount)
            + ", datagrams="
            + std::to_string(attempt.prePunchDatagrams));
        if (!running.load()) {
            return fail(NatPunchAttemptOutcome::Stopped,
                        "Adaptive NAT traversal stopped during pre-punch");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return fail(NatPunchAttemptOutcome::PunchTimeout,
                        "Adaptive NAT traversal timed out during pre-punch");
        }
    }

    const auto barrierStartedAt = std::chrono::steady_clock::now();
    const auto remainingAttemptMs = std::chrono::duration_cast<
        std::chrono::milliseconds>(deadline - barrierStartedAt).count();
    const uint32_t barrierWaitMs = LimitNatPunchBarrierWaitMs(
        static_cast<uint64_t>((std::max)(int64_t{0}, remainingAttemptMs)));
    const auto barrierDeadline = barrierStartedAt
        + std::chrono::milliseconds(barrierWaitMs);
    const auto recordBarrierElapsed = [&]() {
        attempt.barrierElapsedMs = static_cast<uint32_t>((std::max)(
            int64_t{0}, std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - barrierStartedAt).count()));
    };
    nextControlSend = std::chrono::steady_clock::time_point{};
    bool startReceived = false;
    while (running.load()
           && std::chrono::steady_clock::now() < barrierDeadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextControlSend) {
            if (!rendezvous.SendNatArmed(
                    controlSocket, matchedPeerId, session.sessionId,
                    session.attemptId)) {
                Log(LogLevel::Warn, "Failed to send NAT_ARMED. err="
                    + std::to_string(GetSocketError()));
            }
            nextControlSend = now + kControlRetryInterval;
        }

        UdpEndpoint source{};
        int received = -1;
        if (!ReceiveDatagram(controlSocket, &buffer, &source, &received, error)) {
            recordBarrierElapsed();
            return fail(NatPunchAttemptOutcome::SocketError, *error);
        }
        if (received < 0) continue;
        const RendezvousEvent event = rendezvous.HandlePacket(
            source, buffer.data(), static_cast<size_t>(received));
        if (event.type == RendezvousEventType::Error) {
            recordBarrierElapsed();
            return fail(NatPunchAttemptOutcome::ControlError, event.error);
        }
        if (event.type == RendezvousEventType::NatArmedAck
            && MatchesSession(event, session)) {
            attempt.barrierArmedAcknowledged = true;
            continue;
        }
        if (event.type == RendezvousEventType::NatStart
            && MatchesSession(event, session)) {
            startReceived = true;
            break;
        }
    }
    recordBarrierElapsed();
    if (!startReceived) {
        const bool stillRunning = running.load();
        return fail(stillRunning
                        ? NatPunchAttemptOutcome::BarrierTimeout
                        : NatPunchAttemptOutcome::Stopped,
                    stillRunning
                        ? attempt.barrierArmedAcknowledged
                            ? "Timed out at the NAT synchronization barrier; "
                              "local NAT_ARMED was acknowledged but the peer "
                              "did not become ready"
                            : "Timed out at the NAT synchronization barrier; "
                              "the rendezvous did not acknowledge local "
                              "NAT_ARMED"
                        : "Adaptive NAT traversal stopped");
    }

    if (executionPolicy.role == NatPunchExecutionRole::Sender
        && executionPolicy.senderDelayMs > 0) {
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
        attempt.senderDelayMs = LimitNatPunchSenderDelayMs(
            executionPolicy.senderDelayMs,
            static_cast<uint64_t>((std::max)(int64_t{0}, remaining.count())));
        if (attempt.senderDelayMs > 0) {
            Log(LogLevel::Info, "NAT punch sender waiting "
                + std::to_string(attempt.senderDelayMs)
                + "ms for receiver pre-punch");
            if (!WaitForNatPunchDelay(running, attempt.senderDelayMs)) {
                return fail(NatPunchAttemptOutcome::Stopped,
                            "Adaptive NAT traversal stopped during sender delay");
            }
        }
    }
    const auto punchBudget = std::chrono::duration_cast<
        std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    const bool randomSender = plan.mode == NatPunchPlanMode::RandomSender
        || plan.mode == NatPunchPlanMode::MixedRandomSender;
    const bool randomReceiver = plan.mode == NatPunchPlanMode::RandomReceiver
        || plan.mode == NatPunchPlanMode::MixedRandomReceiver;
    const size_t datagramsPerWave = randomReceiver
        ? punchSockets.size() * plan.targets.size()
        : plan.targets.size();
    NatPunchAttemptPolicy remainingPolicy = policy;
    remainingPolicy.datagramBudget = attempt.probeAttempts
            >= policy.datagramBudget
        ? 0 : policy.datagramBudget - attempt.probeAttempts;
    attempt.waveIntervalMs = randomSender
        ? policy.randomPortIntervalMs
        : ComputeNatPunchWaveIntervalMs(
            remainingPolicy, datagramsPerWave,
            static_cast<uint64_t>((std::max)(
                int64_t{1}, punchBudget.count())));
    Log(LogLevel::Info, "NAT punch send interval="
        + std::to_string(attempt.waveIntervalMs)
        + "ms, datagram_budget=" + std::to_string(policy.datagramBudget)
        + ", random_targets="
        + std::to_string(randomSender ? plan.targets.size() : 0)
        + ", receiver_sockets="
        + std::to_string(randomReceiver ? punchSockets.size() : 0));
    NatPunchScheduler scheduler(datagramsPerWave,
        randomSender ? 1 : policy.scanBatchSize,
        randomSender ? policy.randomPortIntervalMs : policy.scanBatchIntervalMs,
        attempt.waveIntervalMs, !randomSender);
    NatPunchHandshake handshake(executionPolicy.role, nonce);
    auto nextAck = std::chrono::steady_clock::time_point{};
    const auto sendReply = [&](NatPunchReply reply) {
        const std::string packet = MakeControlMessage(
            reply == NatPunchReply::Ack ? "PUNCH_ACK" : "PUNCH_CONFIRM",
            {session.sessionId, std::to_string(session.attemptId),
             cfg.peer_id, handshake.nonce(), session.punchToken});
        Send(handshake.socket(), handshake.peer(), packet);
    };
    const auto sendProbe = [&](socket_t socket, size_t targetIndex) {
        const auto result = SendNatPunchProbe(socket, plan.targets[targetIndex], punch,
            lowTtlReceiver ? executionPolicy.prePunchTtl : 0);
        RecordProbe(result, targetIndex, &attempt, &probedTargets);
        if (!result.ttlRestored) {
            *error = "Cannot restore NAT punch socket TTL. err="
                + std::to_string(result.socketError);
            return false;
        }
        return true;
    };
    while (running.load() && std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (handshake.awaitingConfirmation() && now >= nextAck) {
            sendReply(NatPunchReply::Ack);
            nextAck = now + std::chrono::milliseconds(100);
        }
        if (!handshake.awaitingConfirmation()
            && attempt.probeAttempts < policy.datagramBudget) {
            const auto batch = scheduler.TakeBatch(now,
                policy.datagramBudget - attempt.probeAttempts);
            for (size_t i = batch.offset; i < batch.offset + batch.count; ++i) {
                if (!running.load() || std::chrono::steady_clock::now() >= deadline) break;
                const size_t socketIndex = i / plan.targets.size();
                const size_t targetIndex = i % plan.targets.size();
                if (!sendProbe(punchSockets.sockets()[socketIndex], targetIndex)) {
                    return fail(NatPunchAttemptOutcome::SocketError, *error);
                }
            }
        }

        UdpEndpoint source{};
        int received = -1;
        socket_t receivingSocket = kInvalidSocket;
        if (!running.load() || std::chrono::steady_clock::now() >= deadline) break;
        const auto nextEvent = handshake.awaitingConfirmation() ? nextAck
            : attempt.probeAttempts < policy.datagramBudget ? scheduler.nextSend() : deadline;
        const int receiveTimeoutMs = NatPunchPollTimeoutMs(
            std::chrono::steady_clock::now(), nextEvent, deadline, kReceiveTimeoutMs);
        if (!punchSockets.Receive(
                receiveTimeoutMs, &buffer,
                &receivingSocket, &source, &received, error)) {
            return fail(NatPunchAttemptOutcome::SocketError, *error);
        }
        if (received < 0) continue;

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
            || (type != "PUNCH" && type != "PUNCH_ACK"
                && type != "PUNCH_CONFIRM")) {
            continue;
        }
        const NatPunchReply reply = handshake.Handle(
            receivingSocket, source, type, fields[3]);
        if (reply != NatPunchReply::None) {
            sendReply(reply);
            nextAck = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(100);
        }
        if (!handshake.complete()) continue;
        *peer = handshake.peer();
        receivingSocket = punchSockets.ReleaseWinner(handshake.socket());
        if (receivingSocket == kInvalidSocket) {
            return fail(NatPunchAttemptOutcome::SocketError,
                        "NAT punch winner is not owned by the socket pool");
        }
        *punchSocketOut = receivingSocket;
        attempt.outcome = NatPunchAttemptOutcome::Success;
        attempt.confirmedPeer = *peer;
        attempt.detail = "Authenticated PUNCH/ACK/CONFIRM path selected";
        Log(LogLevel::Info, "Adaptive NAT traversal confirmed with "
            + FormatUdpEndpoint(*peer));
        return true;
    }

    const bool stillRunning = running.load();
    return fail(stillRunning ? NatPunchAttemptOutcome::PunchTimeout
                             : NatPunchAttemptOutcome::Stopped,
                stillRunning ? "Adaptive NAT punch timed out"
                             : "Adaptive NAT traversal stopped");
}
