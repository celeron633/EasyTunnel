#pragma once

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

// Builds deterministic plans for easy/easy direct, regular/easy complementary
// sender/range-scanner, and hard/hard regular dual-range scanning. Random and
// multi-public-IP NATs return false until bounded aggressive strategies land.
bool BuildNatPunchPlan(const NatPunchObservation& local,
                       const NatPunchObservation& peer,
                       NatPunchPlan* plan, std::string* error);
const char* NatPunchPlanModeName(NatPunchPlanMode mode);
