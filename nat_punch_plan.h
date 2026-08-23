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
};

struct NatPunchAttemptPolicy {
    uint16_t rangeScale = 1;
    uint16_t maximumRangeSpan = 10;
    uint16_t minimumWaveIntervalMs = 200;
    uint32_t datagramBudget = 16384;
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

// Builds deterministic plans for easy/easy direct, regular/easy complementary
// sender/range-scanner, and hard/hard regular dual-range scanning. Random and
// multi-public-IP NATs return false until bounded random strategies land.
bool BuildNatPunchPlan(const NatPunchObservation& local,
                       const NatPunchObservation& peer,
                       NatPunchPlan* plan, std::string* error);
bool BuildNatPunchPlan(const NatPunchObservation& local,
                       const NatPunchObservation& peer,
                       const NatPunchAttemptPolicy& policy,
                       NatPunchPlan* plan, std::string* error);
const char* NatPunchPlanModeName(NatPunchPlanMode mode);
