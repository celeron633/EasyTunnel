#include <iostream>
#include <string>
#include <vector>

#include "client_config.h"
#include "config.h"

namespace {
int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}
}  // namespace

int main(int argc, char** argv) {
    const auto defaults = DefaultTraversalModes();
    Expect(SerializeTraversalModes(defaults)
               == "nat_punch:true,ipv6:false,ipv4_relay:false",
           "default traversal modes");

    std::vector<TraversalModeSetting> parsed;
    std::string error;
    Expect(ParseTraversalModes(
               "ipv6:true,nat_punch:false,ipv4_relay:true",
               &parsed, &error),
           "custom traversal order parses");
    Expect(parsed.size() == 3 && parsed[0].mode == TraversalMode::Ipv6
               && parsed[2].mode == TraversalMode::Ipv4Relay,
           "custom traversal order is retained");
    Expect(SerializeTraversalModes(parsed)
               == "ipv6:true,nat_punch:false,ipv4_relay:true",
           "custom traversal modes round-trip");

    Config config;
    config.traversal_modes = parsed;
    Expect(IsTraversalModeEnabled(config, TraversalMode::Ipv6),
           "enabled mode lookup");
    Expect(!IsTraversalModeEnabled(config, TraversalMode::NatPunch),
           "disabled mode lookup");

    const auto enabled = EnabledTraversalModes(config.traversal_modes);
    Expect(SerializeTraversalModeSequence(enabled) == "ipv6,ipv4_relay",
           "enabled capabilities retain configured order");
    Expect(SerializeTraversalModeSequence({}) == "none",
           "empty capabilities use explicit wire value");

    std::vector<TraversalMode> capabilities;
    Expect(ParseTraversalModeSequence(
               "nat_punch,ipv4_relay", &capabilities, &error),
           "wire capability sequence parses");
    Expect(capabilities.size() == 2
               && capabilities[0] == TraversalMode::NatPunch
               && capabilities[1] == TraversalMode::Ipv4Relay,
           "wire capability order is retained");
    Expect(ParseTraversalModeSequence("none", &capabilities, &error)
               && capabilities.empty(),
           "empty wire capability sequence parses");
    Expect(!ParseTraversalModeSequence(
               "nat,nat", &capabilities, &error),
           "duplicate wire capability is rejected");

    const std::vector<TraversalMode> initiator{
        TraversalMode::Ipv4Relay,
        TraversalMode::NatPunch,
    };
    const std::vector<TraversalMode> peer{
        TraversalMode::NatPunch,
    };
    const auto negotiated = IntersectTraversalModes(initiator, peer);
    Expect(SerializeTraversalModeSequence(negotiated) == "nat_punch",
           "negotiated modes follow initiator order");
    Expect(IntersectTraversalModes(
               {TraversalMode::Ipv6}, {TraversalMode::Ipv4Relay}).empty(),
           "incompatible capabilities have no negotiated mode");

    Expect(!ParseTraversalModes(
               "nat_punch:true,nat_punch:true,ipv6:false,ipv4_relay:false",
               &parsed, &error),
           "duplicate traversal mode is rejected");
    Expect(!ParseTraversalModes(
               "nat_punch:true,ipv6:false", &parsed, &error),
           "missing traversal mode is rejected");
    Expect(ParseTraversalModes(
               "nat_punch:false,ipv6:false,ipv4_relay:false",
               &parsed, &error),
           "all-disabled modes can be persisted for UI editing");
    Expect(ParseTraversalModes(
               "nat:false,nat4:true,ipv6:false,ipv4_relay:false",
               &parsed, &error)
               && SerializeTraversalModes(parsed)
                    == "nat_punch:true,ipv6:false,ipv4_relay:false",
           "legacy nat/nat4 settings migrate to one adaptive NAT Punch mode");

    Expect(argc == 2, "example config path is provided");
    if (argc == 2) {
        ClientConfig example;
        bool existed = false;
        Expect(LoadClientConfig(argv[1], &example, &existed, &error)
                   && existed,
               "shared example config loads");
        Expect(SerializeTraversalModes(example.traversalModes)
                   == "nat_punch:true,ipv6:false,ipv4_relay:false",
               "shared example traversal strategy");
        Expect(example.stunServers.size() == 2
                   && example.stunServers[0].host == "198.51.100.10"
                   && example.stunServers[1].port == 3478
                   && example.natPunchAttemptLimit == 3,
               "shared example NAT Punch settings");
        Expect(ValidateClientConfig(example, &error),
               "shared example config validates");
        ClientConfig missingStun = example;
        missingStun.stunServers[1].host.clear();
        Expect(!ValidateClientConfig(missingStun, &error),
               "adaptive NAT Punch requires both STUN servers");
        const Config engine = ToEngineConfig(example, "node-b");
        Expect(engine.rendezvous_addr == example.rendezvousAddress
                   && engine.target_peer_id == "node-b"
                   && engine.tun_mtu == 1452
                   && engine.nat_punch_attempt_limit == 3
                   && engine.stun_servers.size() == 2,
               "engine config mapping");
        example.natPunchAttemptLimit = 0;
        Expect(ToEngineConfig(example, "node-b").nat_punch_attempt_limit == 1,
               "NAT punch attempt limit clamps to its minimum");
        example.natPunchAttemptLimit = 99;
        Expect(ToEngineConfig(example, "node-b").nat_punch_attempt_limit == 10,
               "NAT punch attempt limit clamps to its maximum");
    }

    if (failures != 0) {
        std::cerr << failures << " traversal mode test(s) failed\n";
        return 1;
    }
    std::cout << "Traversal mode configuration tests passed\n";
    return 0;
}
