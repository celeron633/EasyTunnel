#include <iostream>
#include <string>
#include <unordered_set>

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
               && balancedSecond.randomReceiverSocketCount == 64
               && balancedSecond.randomTargetPortCount == 512
               && plan.portSpan == 16 && plan.targets.size() == 33,
           "balanced retry doubles the initial regular range");
    const NatPunchAttemptPolicy balancedThird =
        ResolveNatPunchAttemptPolicy(NatPunchProfile::Balanced, 3);
    Expect(BuildNatPunchPlan(
               easyA, regular, balancedThird, &plan, &error)
               && balancedThird.randomReceiverSocketCount == 128
               && balancedThird.randomTargetPortCount == 1000
               && balancedThird.randomPortIntervalMs == 15
               && plan.portSpan == 32 && plan.targets.size() == 65,
           "later balanced retries continue expanding the range");

    const NatPunchAttemptPolicy aggressiveSecond =
        ResolveNatPunchAttemptPolicy(NatPunchProfile::Aggressive, 2);
    Expect(BuildNatPunchPlan(
               easyA, regular, aggressiveSecond, &plan, &error)
               && aggressiveSecond.rangeScale == 4
               && aggressiveSecond.maximumRangeSpan == 128
               && aggressiveSecond.randomReceiverSocketCount == 128
               && aggressiveSecond.randomTargetPortCount == 1000
               && plan.portSpan == 32 && plan.targets.size() == 65,
           "aggressive retry expands the regular range faster");
    const NatPunchAttemptPolicy aggressiveMaximum =
        ResolveNatPunchAttemptPolicy(NatPunchProfile::Aggressive, 10);
    Expect(BuildNatPunchPlan(
               easyA, regular, aggressiveMaximum, &plan, &error)
               && aggressiveMaximum.randomReceiverSocketCount == 256
               && aggressiveMaximum.randomTargetPortCount == 1000
               && aggressiveMaximum.randomPortIntervalMs == 5
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
    Expect(BuildNatPunchPlan(easyA, random, &plan, &error)
               && plan.mode == NatPunchPlanMode::RandomSender
               && plan.targets.empty(),
           "easy side becomes the bounded random-port sender");
    std::vector<UdpEndpoint> randomTargets;
    Expect(BuildRandomPortTargets(random, 256, &randomTargets, &error)
               && randomTargets.size() == 256
               && FormatUdpEndpoint(randomTargets.front())
                    == "198.51.100.40:44000",
           "random sender includes observed ports before random targets");
    std::unordered_set<std::string> uniqueRandomTargets;
    for (const UdpEndpoint& target : randomTargets) {
        uniqueRandomTargets.insert(FormatUdpEndpoint(target));
    }
    Expect(uniqueRandomTargets.size() == randomTargets.size(),
           "random sender targets do not repeat within one attempt");
    Expect(BuildNatPunchPlan(random, easyB, &plan, &error)
               && plan.mode == NatPunchPlanMode::RandomReceiver
               && plan.receiverSocketCount == 32
               && plan.targets.size() == 1
               && FormatUdpEndpoint(plan.targets[0])
                    == "203.0.113.20:41000",
           "random side becomes a bounded multi-socket receiver");
    Expect(BuildNatPunchPlan(regular, random, &plan, &error)
               && plan.mode == NatPunchPlanMode::MixedRandomSender
               && plan.targets.empty()
               && plan.receiverSocketCount == 1,
           "regular side becomes the mixed random-port sender");
    Expect(BuildNatPunchPlan(random, regular, &plan, &error)
               && plan.mode == NatPunchPlanMode::MixedRandomReceiver
               && plan.receiverSocketCount == 32
               && plan.predictedPort == 42006
               && plan.portSpan == 2
               && plan.targets.size() == 5
               && FormatUdpEndpoint(plan.targets.front())
                    == "198.51.100.30:42004"
               && FormatUdpEndpoint(plan.targets.back())
                    == "198.51.100.30:42008",
           "random side opens sockets and scans a small regular range");
    Expect(BuildNatPunchPlan(
               random, regular, balancedThird, &plan, &error)
               && plan.mode == NatPunchPlanMode::MixedRandomReceiver
               && plan.receiverSocketCount == 128
               && plan.portSpan == 8
               && plan.targets.size() == 17,
           "mixed receiver expands sockets and range on balanced retries");
    Expect(BuildNatPunchPlan(
               random, regular, aggressiveMaximum, &plan, &error)
               && plan.receiverSocketCount == 256
               && plan.portSpan == 32
               && plan.targets.size() == 65,
           "mixed receiver remains bounded at the aggressive maximum");
    Expect(!BuildNatPunchPlan(random, random, &plan, &error),
           "random/random remains unsupported");

    const auto decreasing = Observation(
        NatMappingBehavior::PortDependentRegular,
        "198.51.100.50", 50003, 50001);
    Expect(BuildNatPunchPlan(easyA, decreasing, &plan, &error)
               && plan.predictedPort == 49999 && plan.portSpan == 7,
           "decreasing regular mappings predict in the observed direction");
    Expect(BuildNatPunchPlan(random, decreasing, &plan, &error)
               && plan.mode == NatPunchPlanMode::MixedRandomReceiver
               && plan.predictedPort == 49999
               && plan.portSpan == 2
               && FormatUdpEndpoint(plan.targets.front())
                    == "198.51.100.50:49997"
               && FormatUdpEndpoint(plan.targets.back())
                    == "198.51.100.50:50001",
           "mixed receiver supports decreasing regular predictions");

    const auto boundary = Observation(
        NatMappingBehavior::PortDependentRegular,
        "198.51.100.60", 5, 3);
    Expect(BuildNatPunchPlan(easyA, boundary, &plan, &error)
               && plan.predictedPort == 1
               && FormatUdpEndpoint(plan.targets.front())
                    == "198.51.100.60:1",
           "range scan clamps at the minimum UDP port");
    Expect(BuildNatPunchPlan(random, boundary, &plan, &error)
               && plan.mode == NatPunchPlanMode::MixedRandomReceiver
               && plan.predictedPort == 1
               && plan.targets.size() == 3
               && FormatUdpEndpoint(plan.targets.front())
                    == "198.51.100.60:1"
               && FormatUdpEndpoint(plan.targets.back())
                    == "198.51.100.60:3",
           "mixed range clamps at the minimum UDP port");

    const auto multiPublic = NatPunchObservation{
        NatMappingBehavior::MultiPublicIp,
        Endpoint("198.51.100.70", 51000),
        Endpoint("203.0.113.70", 51001)};
    Expect(!BuildNatPunchPlan(multiPublic, easyB, &plan, &error)
               && error.find("multi-public-IP") != std::string::npos,
           "multi-public-IP returns a specific unsupported error");

    const NatPunchExecutionPolicy firstSender =
        ResolveNatPunchExecutionPolicy(
            NatPunchPlanMode::MixedRandomSender, true, 1);
    const NatPunchExecutionPolicy secondSender =
        ResolveNatPunchExecutionPolicy(
            NatPunchPlanMode::MixedRandomSender, true, 2);
    const NatPunchExecutionPolicy secondReceiver =
        ResolveNatPunchExecutionPolicy(
            NatPunchPlanMode::MixedRandomReceiver, false, 2);
    const NatPunchExecutionPolicy thirdReceiver =
        ResolveNatPunchExecutionPolicy(
            NatPunchPlanMode::MixedRandomReceiver, false, 3);
    const NatPunchExecutionPolicy fourthReceiver =
        ResolveNatPunchExecutionPolicy(
            NatPunchPlanMode::MixedRandomReceiver, false, 4);
    Expect(firstSender.role == NatPunchExecutionRole::Sender
               && firstSender.senderDelayMs == 0
               && firstSender.prePunchTtl == 0
               && secondSender.senderDelayMs == 1000
               && secondSender.prePunchTtl == 0
               && secondReceiver.role == NatPunchExecutionRole::Receiver
               && secondReceiver.prePunchTtl == 7
               && secondReceiver.senderDelayMs == 0
               && thirdReceiver.prePunchTtl == 4
               && fourthReceiver.prePunchTtl == 0,
           "attempts rotate baseline, TTL 7 and TTL 4 timing variants");
    Expect(ResolveNatPunchExecutionPolicy(
               NatPunchPlanMode::Direct, true, 2).role
                  == NatPunchExecutionRole::Sender
               && ResolveNatPunchExecutionPolicy(
                   NatPunchPlanMode::Direct, false, 2).role
                  == NatPunchExecutionRole::Receiver
               && ResolveNatPunchExecutionPolicy(
                   NatPunchPlanMode::DualRangeScanner, true, 3).senderDelayMs
                  == 1000
               && ResolveNatPunchExecutionPolicy(
                   NatPunchPlanMode::DualRangeScanner, false, 3).prePunchTtl
                  == 4
               && ResolveNatPunchExecutionPolicy(
                   NatPunchPlanMode::RegularSender, false, 2).role
                  == NatPunchExecutionRole::Sender
               && ResolveNatPunchExecutionPolicy(
                   NatPunchPlanMode::RangeScanner, true, 2).role
                  == NatPunchExecutionRole::Receiver,
           "symmetric plans use session role and asymmetric plans keep fixed roles");
    Expect(std::string(NatPunchExecutionRoleName(
                      NatPunchExecutionRole::Sender)) == "sender"
               && std::string(NatPunchExecutionRoleName(
                      NatPunchExecutionRole::Receiver)) == "receiver"
               && LimitNatPunchSenderDelayMs(1000, 8000) == 1000
               && LimitNatPunchSenderDelayMs(1000, 2000) == 500
               && LimitNatPunchSenderDelayMs(1000, 3) == 0,
           "sender delay preserves at least three quarters of attempt time");

    if (failures != 0) {
        std::cerr << failures << " NAT punch plan test(s) failed\n";
        return 1;
    }
    std::cout << "NAT punch plan tests passed\n";
    return 0;
}
