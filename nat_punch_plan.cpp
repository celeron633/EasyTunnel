#include "nat_punch_plan.h"

#include <algorithm>
#include <cstdlib>

namespace {
constexpr int kRegularPortDeltaLimit = 5;
constexpr int kRangePredictionMargin = 5;
constexpr int kMaximumRangeSpan = 10;

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
}  // namespace

bool BuildNatPunchPlan(const NatPunchObservation& local,
                       const NatPunchObservation& peer,
                       NatPunchPlan* plan, std::string* error) {
    if (plan == nullptr) {
        SetError(error, "NAT punch plan output is required");
        return false;
    }
    plan->targets.clear();
    plan->predictedPort = 0;
    plan->portSpan = 0;

    if (!IsSupportedBehavior(local.behavior)
        || !IsSupportedBehavior(peer.behavior)) {
        SetError(error, "Random or multi-public-IP NAT plan is not implemented yet");
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

    if (peer.behavior == NatMappingBehavior::EndpointIndependent) {
        plan->mode = local.behavior == NatMappingBehavior::PortDependentRegular
            ? NatPunchPlanMode::RegularSender : NatPunchPlanMode::Direct;
        plan->predictedPort = peerPortB;
        plan->targets.push_back(peer.mappedB);
        return true;
    }

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

    plan->mode = local.behavior == NatMappingBehavior::PortDependentRegular
        ? NatPunchPlanMode::DualRangeScanner
        : NatPunchPlanMode::RangeScanner;
    plan->predictedPort = predicted;
    plan->portSpan = static_cast<uint16_t>((std::min)(
        kMaximumRangeSpan, absoluteDelta + kRangePredictionMargin));
    const int firstPort = (std::max)(1, predicted - plan->portSpan);
    const int lastPort = (std::min)(65535, predicted + plan->portSpan);
    plan->targets.reserve(static_cast<size_t>(lastPort - firstPort + 1));
    for (int port = firstPort; port <= lastPort; ++port) {
        UdpEndpoint target{};
        char addressText[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &peerAddressB, addressText,
                      sizeof(addressText)) == nullptr
            || !ParseUdpEndpoint(addressText, static_cast<uint16_t>(port),
                                 &target)) {
            SetError(error, "Cannot build a predicted peer endpoint");
            plan->targets.clear();
            return false;
        }
        plan->targets.push_back(target);
    }
    return true;
}

const char* NatPunchPlanModeName(NatPunchPlanMode mode) {
    switch (mode) {
        case NatPunchPlanMode::Direct: return "direct";
        case NatPunchPlanMode::RegularSender: return "regular-sender";
        case NatPunchPlanMode::RangeScanner: return "range-scanner";
        case NatPunchPlanMode::DualRangeScanner: return "dual-range-scanner";
        default: return "unknown";
    }
}
