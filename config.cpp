#include "config.h"

#include <algorithm>
#include <string>

namespace {

std::string Trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) {
        return "";
    }
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

}  // namespace

const char* NatPunchProfileName(NatPunchProfile profile) {
    switch (profile) {
        case NatPunchProfile::Balanced: return "balanced";
        case NatPunchProfile::Aggressive: return "aggressive";
        default: return "balanced";
    }
}

const char* NatPunchProfileDisplayName(NatPunchProfile profile) {
    switch (profile) {
        case NatPunchProfile::Balanced: return "Balanced";
        case NatPunchProfile::Aggressive: return "Aggressive";
        default: return "Balanced";
    }
}

bool ParseNatPunchProfile(const std::string& text,
                          NatPunchProfile* profile) {
    if (profile == nullptr) return false;
    if (text == "balanced") {
        *profile = NatPunchProfile::Balanced;
        return true;
    }
    if (text == "aggressive") {
        *profile = NatPunchProfile::Aggressive;
        return true;
    }
    return false;
}

std::vector<TraversalModeSetting> DefaultTraversalModes() {
    return {
        {TraversalMode::NatPunch, true},
        {TraversalMode::Ipv6, false},
        {TraversalMode::Ipv4Relay, false},
    };
}

const char* TraversalModeName(TraversalMode mode) {
    switch (mode) {
        case TraversalMode::NatPunch: return "nat_punch";
        case TraversalMode::Ipv6: return "ipv6";
        case TraversalMode::Ipv4Relay: return "ipv4_relay";
        default: return "unknown";
    }
}

const char* TraversalModeDisplayName(TraversalMode mode) {
    switch (mode) {
        case TraversalMode::NatPunch: return "Adaptive NAT Punch";
        case TraversalMode::Ipv6: return "IPv6 direct connection";
        case TraversalMode::Ipv4Relay: return "IPv4 traffic relay";
        default: return "Unknown";
    }
}

bool ParseTraversalModes(const std::string& text,
                         std::vector<TraversalModeSetting>* modes,
                         std::string* error) {
    if (modes == nullptr) return false;
    std::vector<TraversalModeSetting> parsed;
    bool sawLegacyNat = false;
    bool sawLegacyNat4 = false;
    bool sawNatPunch = false;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        const std::string item = Trim(text.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start));
        const size_t colon = item.find(':');
        if (colon == std::string::npos || item.find(':', colon + 1) != std::string::npos) {
            if (error) *error = "Each traversal mode must use name:true or name:false";
            return false;
        }
        const std::string name = Trim(item.substr(0, colon));
        const std::string enabledText = Trim(item.substr(colon + 1));
        TraversalMode mode;
        bool legacyNatMode = false;
        if (name == "nat_punch") {
            if (sawNatPunch || sawLegacyNat || sawLegacyNat4) {
                if (error) *error = "Duplicate NAT Punch traversal mode";
                return false;
            }
            sawNatPunch = true;
            mode = TraversalMode::NatPunch;
        } else if (name == "nat" || name == "nat4") {
            bool& seen = name == "nat" ? sawLegacyNat : sawLegacyNat4;
            if (seen || sawNatPunch) {
                if (error) *error = "Duplicate traversal mode: " + name;
                return false;
            }
            seen = true;
            legacyNatMode = true;
            mode = TraversalMode::NatPunch;
        }
        else if (name == "ipv6") mode = TraversalMode::Ipv6;
        else if (name == "ipv4_relay") mode = TraversalMode::Ipv4Relay;
        else {
            if (error) *error = "Unknown traversal mode: " + name;
            return false;
        }
        bool enabled = false;
        if (enabledText == "true") enabled = true;
        else if (enabledText != "false") {
            if (error) *error = "Traversal mode state must be true or false: " + name;
            return false;
        }
        const auto existing = std::find_if(
            parsed.begin(), parsed.end(), [mode](const auto& value) {
                return value.mode == mode;
            });
        if (existing != parsed.end()) {
            if (!legacyNatMode || existing->mode != TraversalMode::NatPunch) {
                if (error) *error = "Duplicate traversal mode: " + name;
                return false;
            }
            // Legacy nat/nat4 were separate toggles. Either one enabled means
            // the single adaptive NAT Punch mode remains enabled after migration.
            existing->enabled = existing->enabled || enabled;
        } else {
            parsed.push_back({mode, enabled});
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (parsed.size() != 3) {
        if (error) *error = "traversal_modes must contain nat_punch, ipv6 and ipv4_relay exactly once";
        return false;
    }
    *modes = std::move(parsed);
    return true;
}

std::string SerializeTraversalModes(
    const std::vector<TraversalModeSetting>& modes) {
    std::string output;
    for (const auto& mode : modes) {
        if (!output.empty()) output += ',';
        output += TraversalModeName(mode.mode);
        output += mode.enabled ? ":true" : ":false";
    }
    return output;
}

bool IsTraversalModeEnabled(const Config& config, TraversalMode mode) {
    const auto found = std::find_if(
        config.traversal_modes.begin(), config.traversal_modes.end(),
        [mode](const auto& value) { return value.mode == mode; });
    return found != config.traversal_modes.end() && found->enabled;
}

std::vector<TraversalMode> EnabledTraversalModes(
    const std::vector<TraversalModeSetting>& modes) {
    std::vector<TraversalMode> enabled;
    for (const auto& setting : modes) {
        if (setting.enabled) enabled.push_back(setting.mode);
    }
    return enabled;
}

std::string SerializeTraversalModeSequence(
    const std::vector<TraversalMode>& modes) {
    if (modes.empty()) return "none";
    std::string output;
    for (const TraversalMode mode : modes) {
        if (!output.empty()) output += ',';
        output += TraversalModeName(mode);
    }
    return output;
}

bool ParseTraversalModeSequence(const std::string& text,
                                std::vector<TraversalMode>* modes,
                                std::string* error) {
    if (modes == nullptr) return false;
    modes->clear();
    if (text == "none") return true;

    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        const std::string name = Trim(text.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start));
        TraversalMode mode;
        if (name == "nat_punch") mode = TraversalMode::NatPunch;
        else if (name == "ipv6") mode = TraversalMode::Ipv6;
        else if (name == "ipv4_relay") mode = TraversalMode::Ipv4Relay;
        else {
            if (error) *error = "Unknown traversal mode: " + name;
            modes->clear();
            return false;
        }
        if (std::find(modes->begin(), modes->end(), mode) != modes->end()) {
            if (error) *error = "Duplicate traversal mode: " + name;
            modes->clear();
            return false;
        }
        modes->push_back(mode);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return true;
}

std::vector<TraversalMode> IntersectTraversalModes(
    const std::vector<TraversalMode>& preferred,
    const std::vector<TraversalMode>& supported) {
    std::vector<TraversalMode> intersection;
    for (const TraversalMode mode : preferred) {
        if (std::find(supported.begin(), supported.end(), mode)
            != supported.end()) {
            intersection.push_back(mode);
        }
    }
    return intersection;
}
