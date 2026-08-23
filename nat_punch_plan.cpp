#include "nat_punch_plan.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <numeric>

#include "secure_random.h"

namespace {
constexpr int kRegularPortDeltaLimit = 5;
constexpr int kRangePredictionMargin = 5;
constexpr int kInitialMaximumRangeSpan = 10;
constexpr uint16_t kInitialMixedRangeSpan = 2;
constexpr uint16_t kFirstRandomPort = 1024;
constexpr uint32_t kRandomPortCount = 65536u - kFirstRandomPort;

bool Ipv4AddressAndPort(const UdpEndpoint& endpoint, in_addr* address,
                        uint16_t* port) {
    if (endpoint.family != AF_INET
        || endpoint.addr_len < static_cast<socket_len_t>(sizeof(sockaddr_in))) {
        return false;
    }
    const auto* value = reinterpret_cast<const sockaddr_in*>(&endpoint.addr);
    *address = value->sin_addr;
    *port = ntohs(value->sin_port);
    return true;
}

void SetError(std::string* error, const std::string& value) {
    if (error != nullptr) *error = value;
}

bool IsSupportedBehavior(NatMappingBehavior behavior) {
    return behavior == NatMappingBehavior::EndpointIndependent
        || behavior == NatMappingBehavior::PortDependentRegular;
}

bool EndpointFromAddress(in_addr address, uint16_t port,
                         UdpEndpoint* endpoint) {
    char addressText[INET_ADDRSTRLEN]{};
    return inet_ntop(AF_INET, &address, addressText,
                     sizeof(addressText)) != nullptr
        && ParseUdpEndpoint(addressText, port, endpoint);
}

bool BuildPredictedRange(in_addr peerAddress, uint16_t peerPortA,
                         uint16_t peerPortB, uint16_t span,
                         NatPunchPlan* plan, std::string* error) {
    const int delta = static_cast<int>(peerPortB)
        - static_cast<int>(peerPortA);
    const int absoluteDelta = std::abs(delta);
    if (delta == 0 || absoluteDelta > kRegularPortDeltaLimit) {
        SetError(error, "Peer regular NAT observation has an invalid port delta");
        return false;
    }
    const int predicted = static_cast<int>(peerPortB) + delta;
    if (predicted < 1 || predicted > 65535) {
        SetError(error, "Predicted peer port is outside the UDP port range");
        return false;
    }

    plan->predictedPort = predicted;
    plan->portSpan = span;
    const int firstPort = (std::max)(1, predicted - plan->portSpan);
    const int lastPort = (std::min)(65535, predicted + plan->portSpan);
    plan->targets.reserve(static_cast<size_t>(lastPort - firstPort + 1));
    for (int port = firstPort; port <= lastPort; ++port) {
        UdpEndpoint target{};
        if (!EndpointFromAddress(
                peerAddress, static_cast<uint16_t>(port), &target)) {
            SetError(error, "Cannot build a predicted peer endpoint");
            plan->targets.clear();
            return false;
        }
        plan->targets.push_back(target);
    }
    return true;
}
}  // namespace

NatPunchAttemptPolicy ResolveNatPunchAttemptPolicy(
        NatPunchProfile profile, uint16_t attemptNumber) {
    const uint16_t attempt = (std::max)(uint16_t{1}, attemptNumber);
    NatPunchAttemptPolicy policy;
    if (profile == NatPunchProfile::Aggressive) {
        const unsigned exponent = attempt == 1
            ? 0u : (std::min)(static_cast<unsigned>(attempt), 4u);
        policy.rangeScale = static_cast<uint16_t>(1u << exponent);
        policy.maximumRangeSpan = 128;
        policy.minimumWaveIntervalMs = 75;
        policy.datagramBudget = 131072;
        const unsigned resourceShift = (std::min)(
            static_cast<unsigned>(attempt), 3u);
        policy.randomReceiverSocketCount = static_cast<uint16_t>((std::min)(
            256u, 32u << resourceShift));
        policy.randomTargetPortCount = static_cast<uint16_t>((std::min)(
            1000u, 256u << resourceShift));
        policy.randomPortIntervalMs = 5;
        return policy;
    }

    const unsigned exponent = (std::min)(
        static_cast<unsigned>(attempt - 1), 3u);
    policy.rangeScale = static_cast<uint16_t>(1u << exponent);
    policy.maximumRangeSpan = 48;
    policy.minimumWaveIntervalMs = 200;
    policy.datagramBudget = 16384;
    const unsigned resourceShift = (std::min)(
        static_cast<unsigned>(attempt), 3u);
    policy.randomReceiverSocketCount = static_cast<uint16_t>((std::min)(
        128u, 16u << resourceShift));
    policy.randomTargetPortCount = static_cast<uint16_t>((std::min)(
        1000u, 128u << resourceShift));
    policy.randomPortIntervalMs = 15;
    return policy;
}

uint32_t ComputeNatPunchWaveIntervalMs(
        const NatPunchAttemptPolicy& policy, size_t targetCount,
        uint64_t punchBudgetMs) {
    if (targetCount == 0 || punchBudgetMs == 0
        || policy.datagramBudget == 0) {
        return policy.minimumWaveIntervalMs;
    }
    const uint64_t allowedWaves = (std::max)(
        uint64_t{1}, static_cast<uint64_t>(policy.datagramBudget)
            / static_cast<uint64_t>(targetCount));
    const uint64_t budgetLimitedInterval =
        (punchBudgetMs + allowedWaves - 1) / allowedWaves;
    const uint64_t interval = (std::max)(
        static_cast<uint64_t>(policy.minimumWaveIntervalMs),
        budgetLimitedInterval);
    return static_cast<uint32_t>((std::min)(
        interval, static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())));
}

bool BuildRandomPortTargets(const NatPunchObservation& peer,
                            size_t targetCount,
                            std::vector<UdpEndpoint>* targets,
                            std::string* error) {
    if (targets == nullptr || targetCount == 0
        || targetCount > kRandomPortCount) {
        SetError(error, "Random NAT target count is outside the safe range");
        return false;
    }
    in_addr peerAddressA{};
    in_addr peerAddressB{};
    uint16_t peerPortA = 0;
    uint16_t peerPortB = 0;
    if (!Ipv4AddressAndPort(peer.mappedA, &peerAddressA, &peerPortA)
        || !Ipv4AddressAndPort(peer.mappedB, &peerAddressB, &peerPortB)
        || peerAddressA.s_addr != peerAddressB.s_addr) {
        SetError(error, "Random NAT peer mappings do not share one public IPv4 address");
        return false;
    }

    targets->clear();
    targets->reserve(targetCount);
    std::vector<bool> used(65536, false);
    const auto append = [&](uint16_t port) {
        if (targets->size() >= targetCount || used[port]) return true;
        UdpEndpoint endpoint{};
        if (!EndpointFromAddress(peerAddressB, port, &endpoint)) return false;
        used[port] = true;
        targets->push_back(endpoint);
        return true;
    };
    if (!append(peerPortB) || !append(peerPortA)) {
        SetError(error, "Cannot build an observed random NAT endpoint");
        targets->clear();
        return false;
    }

    uint32_t entropy[2]{};
    if (!FillSecureRandom(reinterpret_cast<uint8_t*>(entropy),
                          sizeof(entropy), error)) {
        targets->clear();
        return false;
    }
    const uint32_t start = entropy[0] % kRandomPortCount;
    uint32_t step = entropy[1] % kRandomPortCount;
    if (step == 0) step = 1;
    while (std::gcd(step, kRandomPortCount) != 1) {
        step = step == kRandomPortCount - 1 ? 1 : step + 1;
    }
    for (uint32_t index = 0;
         index < kRandomPortCount && targets->size() < targetCount;
         ++index) {
        const uint16_t port = static_cast<uint16_t>(kFirstRandomPort
            + (static_cast<uint64_t>(start)
               + static_cast<uint64_t>(index) * step) % kRandomPortCount);
        if (!append(port)) {
            SetError(error, "Cannot build a random NAT target endpoint");
            targets->clear();
            return false;
        }
    }
    if (targets->size() != targetCount) {
        SetError(error, "Cannot generate enough unique random NAT targets");
        targets->clear();
        return false;
    }
    return true;
}

bool BuildNatPunchPlan(const NatPunchObservation& local,
                       const NatPunchObservation& peer,
                       NatPunchPlan* plan, std::string* error) {
    return BuildNatPunchPlan(
        local, peer,
        ResolveNatPunchAttemptPolicy(NatPunchProfile::Balanced, 1),
        plan, error);
}

bool BuildNatPunchPlan(const NatPunchObservation& local,
                       const NatPunchObservation& peer,
                       const NatPunchAttemptPolicy& policy,
                       NatPunchPlan* plan, std::string* error) {
    if (plan == nullptr) {
        SetError(error, "NAT punch plan output is required");
        return false;
    }
    plan->targets.clear();
    plan->predictedPort = 0;
    plan->portSpan = 0;
    plan->receiverSocketCount = 1;
    if (local.behavior == NatMappingBehavior::MultiPublicIp
        || peer.behavior == NatMappingBehavior::MultiPublicIp) {
        SetError(error, "NAT mapping combination with multi-public-IP is unsupported");
        return false;
    }
    if (local.behavior == NatMappingBehavior::Unknown
        || peer.behavior == NatMappingBehavior::Unknown) {
        SetError(error, "NAT mapping behavior is unknown");
        return false;
    }

    in_addr peerAddressA{};
    in_addr peerAddressB{};
    uint16_t peerPortA = 0;
    uint16_t peerPortB = 0;
    if (!Ipv4AddressAndPort(peer.mappedA, &peerAddressA, &peerPortA)
        || !Ipv4AddressAndPort(peer.mappedB, &peerAddressB, &peerPortB)
        || peerAddressA.s_addr != peerAddressB.s_addr) {
        SetError(error, "Peer STUN mappings do not share one public IPv4 address");
        return false;
    }

    const bool localRandom =
        local.behavior == NatMappingBehavior::PortDependentRandom;
    const bool peerRandom =
        peer.behavior == NatMappingBehavior::PortDependentRandom;
    const bool localEasy =
        local.behavior == NatMappingBehavior::EndpointIndependent;
    const bool peerEasy =
        peer.behavior == NatMappingBehavior::EndpointIndependent;
    if (localRandom && peerEasy) {
        plan->mode = NatPunchPlanMode::RandomReceiver;
        plan->predictedPort = peerPortB;
        plan->receiverSocketCount = policy.randomReceiverSocketCount;
        plan->targets.push_back(peer.mappedB);
        return true;
    }
    if (localEasy && peerRandom) {
        plan->mode = NatPunchPlanMode::RandomSender;
        plan->receiverSocketCount = 1;
        return true;
    }
    const bool localRegular =
        local.behavior == NatMappingBehavior::PortDependentRegular;
    const bool peerRegular =
        peer.behavior == NatMappingBehavior::PortDependentRegular;
    if (localRegular && peerRandom) {
        plan->mode = NatPunchPlanMode::MixedRandomSender;
        return true;
    }
    if (localRandom && peerRegular) {
        plan->mode = NatPunchPlanMode::MixedRandomReceiver;
        plan->receiverSocketCount = policy.randomReceiverSocketCount;
        const uint16_t mixedSpan = static_cast<uint16_t>((std::min)(
            static_cast<uint32_t>(policy.maximumRangeSpan),
            static_cast<uint32_t>(policy.rangeScale)
                * kInitialMixedRangeSpan));
        return BuildPredictedRange(peerAddressB, peerPortA, peerPortB,
                                   mixedSpan, plan, error);
    }
    if (!IsSupportedBehavior(local.behavior)
        || !IsSupportedBehavior(peer.behavior)) {
        SetError(error, "NAT mapping combination requires an unsupported random/random strategy");
        return false;
    }

    if (peer.behavior == NatMappingBehavior::EndpointIndependent) {
        plan->mode = local.behavior == NatMappingBehavior::PortDependentRegular
            ? NatPunchPlanMode::RegularSender : NatPunchPlanMode::Direct;
        plan->predictedPort = peerPortB;
        plan->targets.push_back(peer.mappedB);
        return true;
    }

    plan->mode = local.behavior == NatMappingBehavior::PortDependentRegular
        ? NatPunchPlanMode::DualRangeScanner
        : NatPunchPlanMode::RangeScanner;
    const int absoluteDelta = std::abs(
        static_cast<int>(peerPortB) - static_cast<int>(peerPortA));
    const uint32_t initialSpan = static_cast<uint32_t>((std::min)(
        kInitialMaximumRangeSpan, absoluteDelta + kRangePredictionMargin));
    const uint32_t scaledSpan = initialSpan * policy.rangeScale;
    const uint16_t rangeSpan = static_cast<uint16_t>((std::min)(
        scaledSpan, static_cast<uint32_t>(policy.maximumRangeSpan)));
    return BuildPredictedRange(peerAddressB, peerPortA, peerPortB,
                               rangeSpan, plan, error);
}

const char* NatPunchPlanModeName(NatPunchPlanMode mode) {
    switch (mode) {
        case NatPunchPlanMode::Direct: return "direct";
        case NatPunchPlanMode::RegularSender: return "regular-sender";
        case NatPunchPlanMode::RangeScanner: return "range-scanner";
        case NatPunchPlanMode::DualRangeScanner: return "dual-range-scanner";
        case NatPunchPlanMode::RandomSender: return "random-sender";
        case NatPunchPlanMode::RandomReceiver: return "random-receiver";
        case NatPunchPlanMode::MixedRandomSender:
            return "mixed-random-sender";
        case NatPunchPlanMode::MixedRandomReceiver:
            return "mixed-random-receiver";
        default: return "unknown";
    }
}
