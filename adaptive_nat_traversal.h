#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "config.h"
#include "rendezvous_client.h"
#include "stun_client.h"
#include "util.h"

enum class NatPunchAttemptOutcome {
    InvalidInput,
    SocketError,
    StunTimeout,
    StunFailure,
    PeerInfoTimeout,
    StrategyUnsupported,
    BarrierTimeout,
    PunchTimeout,
    ControlError,
    Stopped,
    Success,
};

struct NatPunchAttemptResult {
    std::string sessionId;
    uint64_t attemptId = 0;
    uint16_t attemptNumber = 1;
    std::string localPeerId;
    std::string remotePeerId;
    NatPunchRole role = NatPunchRole::Unknown;
    NatPunchProfile profile = NatPunchProfile::Balanced;
    NatPunchAttemptOutcome outcome = NatPunchAttemptOutcome::InvalidInput;
    NatMappingBehavior localBehavior = NatMappingBehavior::Unknown;
    NatMappingBehavior peerBehavior = NatMappingBehavior::Unknown;
    std::string plan = "-";
    size_t targetCount = 0;
    size_t socketCount = 1;
    uint16_t portSpan = 0;
    std::string executionRole = "-";
    uint8_t prePunchTtl = 0;
    uint32_t senderDelayMs = 0;
    uint32_t prePunchDatagrams = 0;
    uint32_t waveIntervalMs = 0;
    uint32_t datagramBudget = 0;
    uint32_t datagramsSent = 0;
    UdpEndpoint confirmedPeer{};
    int64_t elapsedMs = 0;
    std::string detail;
};

const char* NatPunchAttemptOutcomeName(NatPunchAttemptOutcome outcome);
bool IsRetryableNatPunchOutcome(NatPunchAttemptOutcome outcome);
std::string FormatNatPunchAttemptResult(
    const NatPunchAttemptResult& result);

// Synchronizes a new attempt ID and punch token through the rendezvous
// control socket after both peers finish a retryable failed attempt.
bool RequestNextNatPunchAttempt(socket_t sock, const Config& cfg,
                                const UdpEndpoint& server,
                                const std::atomic<bool>& running,
                                const std::string& matchedPeerId,
                                NatPunchSession* session,
                                std::string* error);

// Runs the STUN-based adaptive NAT punch on a dedicated UDP socket. The
// existing rendezvous socket is replaced only after the punch succeeds; on
// failure it remains available to subsequent IPv6/relay strategies.
bool PunchAdaptiveNat(socket_t* sock, const Config& cfg,
                      const UdpEndpoint& server,
                      const std::atomic<bool>& running,
                      const std::string& matchedPeerId,
                      const NatPunchSession& session,
                      UdpEndpoint* peer, std::string* error,
                      NatPunchAttemptResult* attemptResult = nullptr,
                      uint16_t attemptNumber = 1);
