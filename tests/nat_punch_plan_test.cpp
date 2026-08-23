#include <iostream>
#include <string>

#include "nat_punch_plan.h"

namespace {
int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}

UdpEndpoint Endpoint(const char* ip, uint16_t port) {
    UdpEndpoint endpoint{};
    ParseUdpEndpoint(ip, port, &endpoint);
    return endpoint;
}

NatPunchObservation Observation(NatMappingBehavior behavior, const char* ip,
                                uint16_t firstPort, uint16_t secondPort) {
    return {behavior, Endpoint(ip, firstPort), Endpoint(ip, secondPort)};
}
}  // namespace

int main() {
    std::string error;
    NatPunchPlan plan;
    const auto easyA = Observation(
        NatMappingBehavior::EndpointIndependent,
        "198.51.100.10", 40000, 40000);
    const auto easyB = Observation(
        NatMappingBehavior::EndpointIndependent,
        "203.0.113.20", 41000, 41000);
    Expect(BuildNatPunchPlan(easyA, easyB, &plan, &error)
               && plan.mode == NatPunchPlanMode::Direct
               && plan.targets.size() == 1
               && FormatUdpEndpoint(plan.targets[0]) == "203.0.113.20:41000",
           "easy/easy uses the peer's stable endpoint");

    const auto regular = Observation(
        NatMappingBehavior::PortDependentRegular,
        "198.51.100.30", 42000, 42003);
    Expect(BuildNatPunchPlan(easyA, regular, &plan, &error)
               && plan.mode == NatPunchPlanMode::RangeScanner
               && plan.predictedPort == 42006 && plan.portSpan == 8
               && plan.targets.size() == 17
               && FormatUdpEndpoint(plan.targets.front())
                    == "198.51.100.30:41998"
               && FormatUdpEndpoint(plan.targets.back())
                    == "198.51.100.30:42014",
           "easy peer scans a dynamic range around the regular prediction");

    const NatPunchAttemptPolicy balancedSecond =
        ResolveNatPunchAttemptPolicy(NatPunchProfile::Balanced, 2);
    Expect(BuildNatPunchPlan(
               easyA, regular, balancedSecond, &plan, &error)
               && balancedSecond.rangeScale == 2
               && balancedSecond.maximumRangeSpan == 48
               && plan.portSpan == 16 && plan.targets.size() == 33,
           "balanced retry doubles the initial regular range");
    const NatPunchAttemptPolicy balancedThird =
        ResolveNatPunchAttemptPolicy(NatPunchProfile::Balanced, 3);
    Expect(BuildNatPunchPlan(
               easyA, regular, balancedThird, &plan, &error)
               && plan.portSpan == 32 && plan.targets.size() == 65,
           "later balanced retries continue expanding the range");

    const NatPunchAttemptPolicy aggressiveSecond =
        ResolveNatPunchAttemptPolicy(NatPunchProfile::Aggressive, 2);
    Expect(BuildNatPunchPlan(
               easyA, regular, aggressiveSecond, &plan, &error)
               && aggressiveSecond.rangeScale == 4
               && aggressiveSecond.maximumRangeSpan == 128
               && plan.portSpan == 32 && plan.targets.size() == 65,
           "aggressive retry expands the regular range faster");
    const NatPunchAttemptPolicy aggressiveMaximum =
        ResolveNatPunchAttemptPolicy(NatPunchProfile::Aggressive, 10);
    Expect(BuildNatPunchPlan(
               easyA, regular, aggressiveMaximum, &plan, &error)
               && plan.portSpan == 128 && plan.targets.size() == 257,
           "aggressive range remains bounded at 257 target ports");
    Expect(ComputeNatPunchWaveIntervalMs(
               balancedThird, 65, 30000) == 200
               && ComputeNatPunchWaveIntervalMs(
                   aggressiveMaximum, 257, 30000) == 75,
           "default timeout uses each profile's wave interval");
    const uint32_t longInterval = ComputeNatPunchWaveIntervalMs(
        aggressiveMaximum, 257, 600000);
    const uint64_t longWaves = (600000 + longInterval - 1) / longInterval;
    Expect(longInterval > aggressiveMaximum.minimumWaveIntervalMs
               && longWaves * 257 <= aggressiveMaximum.datagramBudget,
           "long timeouts slow down to preserve the datagram budget");

    Expect(BuildNatPunchPlan(regular, easyB, &plan, &error)
               && plan.mode == NatPunchPlanMode::RegularSender
               && plan.targets.size() == 1
               && FormatUdpEndpoint(plan.targets[0]) == "203.0.113.20:41000",
           "regular peer sends to the easy peer's stable endpoint");

    const auto regularPeer = Observation(
        NatMappingBehavior::PortDependentRegular,
        "203.0.113.30", 43000, 43002);
    Expect(BuildNatPunchPlan(regular, regularPeer, &plan, &error)
               && plan.mode == NatPunchPlanMode::DualRangeScanner
               && plan.predictedPort == 43004 && plan.portSpan == 7
               && plan.targets.size() == 15
               && FormatUdpEndpoint(plan.targets.front())
                    == "203.0.113.30:42997"
               && FormatUdpEndpoint(plan.targets.back())
                    == "203.0.113.30:43011",
           "hard/hard regular scans the peer's predicted range");
    Expect(BuildNatPunchPlan(regularPeer, regular, &plan, &error)
               && plan.mode == NatPunchPlanMode::DualRangeScanner
               && plan.predictedPort == 42006 && plan.portSpan == 8
               && plan.targets.size() == 17,
           "hard/hard regular produces the complementary peer plan");
    const auto random = Observation(
        NatMappingBehavior::PortDependentRandom,
        "198.51.100.40", 43000, 44000);
    Expect(!BuildNatPunchPlan(easyA, random, &plan, &error),
           "random mapping does not silently use a fixed offset");

    const auto decreasing = Observation(
        NatMappingBehavior::PortDependentRegular,
        "198.51.100.50", 50003, 50001);
    Expect(BuildNatPunchPlan(easyA, decreasing, &plan, &error)
               && plan.predictedPort == 49999 && plan.portSpan == 7,
           "decreasing regular mappings predict in the observed direction");

    const auto boundary = Observation(
        NatMappingBehavior::PortDependentRegular,
        "198.51.100.60", 5, 3);
    Expect(BuildNatPunchPlan(easyA, boundary, &plan, &error)
               && plan.predictedPort == 1
               && FormatUdpEndpoint(plan.targets.front())
                    == "198.51.100.60:1",
           "range scan clamps at the minimum UDP port");

    if (failures != 0) {
        std::cerr << failures << " NAT punch plan test(s) failed\n";
        return 1;
    }
    std::cout << "NAT punch plan tests passed\n";
    return 0;
}
