#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "stun_client.h"
#include "util.h"

enum class NatPunchPlanMode {
    Direct,
    RegularSender,
    RangeScanner,
    DualRangeScanner,
    RandomSender,
    RandomReceiver,
    MixedRandomSender,
    MixedRandomReceiver,
};

struct NatPunchObservation {
    NatMappingBehavior behavior = NatMappingBehavior::Unknown;
    UdpEndpoint mappedA{};
    UdpEndpoint mappedB{};
};

struct NatPunchPlan {
    NatPunchPlanMode mode = NatPunchPlanMode::Direct;
    std::vector<UdpEndpoint> targets;
    int predictedPort = 0;
    uint16_t portSpan = 0;
    uint16_t receiverSocketCount = 1;
};

struct NatPunchAttemptPolicy {
    uint16_t rangeScale = 1;
    uint16_t maximumRangeSpan = 10;
    uint16_t minimumWaveIntervalMs = 200;
    uint32_t datagramBudget = 16384;
    uint16_t randomReceiverSocketCount = 32;
    uint16_t randomTargetPortCount = 256;
    uint16_t randomPortIntervalMs = 15;
};

// Converts the user-facing resource profile and one-based local attempt
// number into deterministic range and send limits.
NatPunchAttemptPolicy ResolveNatPunchAttemptPolicy(
    NatPunchProfile profile, uint16_t attemptNumber);

// Slows full-range waves when necessary so a long punch timeout still stays
// within the profile's per-attempt datagram budget.
uint32_t ComputeNatPunchWaveIntervalMs(
    const NatPunchAttemptPolicy& policy, size_t targetCount,
    uint64_t punchBudgetMs);

// Builds a unique, CSPRNG-seeded target sequence on the peer's observed
// public IPv4. Known STUN ports are tried first; remaining ports are sampled
// without replacement from the non-privileged UDP range.
bool BuildRandomPortTargets(const NatPunchObservation& peer,
                            size_t targetCount,
                            std::vector<UdpEndpoint>* targets,
                            std::string* error);

// Builds deterministic plans for easy/easy direct, regular/easy complementary
// sender/range-scanner, hard/hard regular dual-range scanning, and the
// easy/random complementary random sender/receiver strategy, and the
// regular/random mixed strategy. random/random and multi-public-IP
// combinations remain unsupported.
bool BuildNatPunchPlan(const NatPunchObservation& local,
                       const NatPunchObservation& peer,
                       NatPunchPlan* plan, std::string* error);
bool BuildNatPunchPlan(const NatPunchObservation& local,
                       const NatPunchObservation& peer,
                       const NatPunchAttemptPolicy& policy,
                       NatPunchPlan* plan, std::string* error);
const char* NatPunchPlanModeName(NatPunchPlanMode mode);
