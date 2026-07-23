#include <iostream>
#include <string>
#include <vector>

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
               == "nat:true,nat4:true,ipv6:false,ipv4_relay:false",
           "default traversal modes");

    std::vector<TraversalModeSetting> parsed;
    std::string error;
    Expect(ParseTraversalModes(
               "ipv6:true,nat:false,ipv4_relay:true,nat4:false",
               &parsed, &error),
           "custom traversal order parses");
    Expect(parsed.size() == 4 && parsed[0].mode == TraversalMode::Ipv6
               && parsed[2].mode == TraversalMode::Ipv4Relay,
           "custom traversal order is retained");
    Expect(SerializeTraversalModes(parsed)
               == "ipv6:true,nat:false,ipv4_relay:true,nat4:false",
           "custom traversal modes round-trip");

    Config config;
    config.traversal_modes = parsed;
    Expect(IsTraversalModeEnabled(config, TraversalMode::Ipv6),
           "enabled mode lookup");
    Expect(!IsTraversalModeEnabled(config, TraversalMode::Nat4),
           "disabled mode lookup");

    Expect(!ParseTraversalModes(
               "nat:true,nat:true,ipv6:false,ipv4_relay:false",
               &parsed, &error),
           "duplicate traversal mode is rejected");
    Expect(!ParseTraversalModes(
               "nat:true,nat4:true,ipv6:false", &parsed, &error),
           "missing traversal mode is rejected");
    Expect(ParseTraversalModes(
               "nat:false,nat4:false,ipv6:false,ipv4_relay:false",
               &parsed, &error),
           "all-disabled modes can be persisted for UI editing");

    Expect(argc == 2, "example config path is provided");
    if (argc == 2) {
        Config example;
        Expect(LoadConfig(argv[1], &example), "console example config loads");
        Expect(SerializeTraversalModes(example.traversal_modes)
                   == "nat:true,nat4:true,ipv6:false,ipv4_relay:false",
               "console example traversal strategy");
    }

    if (failures != 0) {
        std::cerr << failures << " traversal mode test(s) failed\n";
        return 1;
    }
    std::cout << "Traversal mode configuration tests passed\n";
    return 0;
}
