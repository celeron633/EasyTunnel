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

// Builds the currently supported deterministic plans: easy/easy direct and
// regular/easy complementary sender/range-scanner. Random and hard/hard NATs
// intentionally return false until their bounded aggressive strategies land.
bool BuildNatPunchPlan(const NatPunchObservation& local,
                       const NatPunchObservation& peer,
                       NatPunchPlan* plan, std::string* error);
const char* NatPunchPlanModeName(NatPunchPlanMode mode);

