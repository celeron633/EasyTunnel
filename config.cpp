#include "config.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>

#include "log.h"

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

bool ParseBool(const std::string& v) {
    const std::string t = Trim(v);
    return t == "1" || t == "true" || t == "TRUE" || t == "yes" || t == "on";
}

bool ParseUInt16(const std::string& text, uint16_t* out) {
    if (out == nullptr) {
        return false;
    }
    try {
        size_t idx = 0;
        const unsigned long v = std::stoul(text, &idx);
        if (idx != text.size()) return false;
        if (v > (std::numeric_limits<uint16_t>::max)()) {
            return false;
        }
        *out = static_cast<uint16_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseUInt8(const std::string& text, uint8_t* out) {
    if (out == nullptr) {
        return false;
    }
    try {
        size_t idx = 0;
        const unsigned long v = std::stoul(text, &idx);
        if (idx != text.size()) return false;
        if (v > (std::numeric_limits<uint8_t>::max)()) {
            return false;
        }
        *out = static_cast<uint8_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

std::vector<TraversalModeSetting> DefaultTraversalModes() {
    return {
        {TraversalMode::Nat, true},
        {TraversalMode::Nat4, true},
        {TraversalMode::Ipv6, false},
        {TraversalMode::Ipv4Relay, false},
    };
}

const char* TraversalModeName(TraversalMode mode) {
    switch (mode) {
        case TraversalMode::Nat: return "nat";
        case TraversalMode::Nat4: return "nat4";
        case TraversalMode::Ipv6: return "ipv6";
        case TraversalMode::Ipv4Relay: return "ipv4_relay";
        default: return "unknown";
    }
}

const char* TraversalModeDisplayName(TraversalMode mode) {
    switch (mode) {
        case TraversalMode::Nat: return "NAT traversal";
        case TraversalMode::Nat4: return "Enhanced NAT4 traversal";
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
        if (name == "nat") mode = TraversalMode::Nat;
        else if (name == "nat4") mode = TraversalMode::Nat4;
        else if (name == "ipv6") mode = TraversalMode::Ipv6;
        else if (name == "ipv4_relay") mode = TraversalMode::Ipv4Relay;
        else {
            if (error) *error = "Unknown traversal mode: " + name;
            return false;
        }
        if (std::any_of(parsed.begin(), parsed.end(), [mode](const auto& value) {
                return value.mode == mode;
            })) {
            if (error) *error = "Duplicate traversal mode: " + name;
            return false;
        }
        bool enabled = false;
        if (enabledText == "true") enabled = true;
        else if (enabledText != "false") {
            if (error) *error = "Traversal mode state must be true or false: " + name;
            return false;
        }
        parsed.push_back({mode, enabled});
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (parsed.size() != 4) {
        if (error) *error = "traversal_modes must contain nat, nat4, ipv6 and ipv4_relay exactly once";
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
        if (name == "nat") mode = TraversalMode::Nat;
        else if (name == "nat4") mode = TraversalMode::Nat4;
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

bool LoadConfig(const std::string& file, Config* out) {
    std::ifstream in(file);
    if (!in.is_open()) {
        Log(LogLevel::Error, "Failed to open config file: " + file);
        return false;
    }

    std::unordered_map<std::string, std::string> kv;
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        const std::string t = Trim(line);
        if (t.empty() || t[0] == '#') {
            continue;
        }
        const auto eq = t.find('=');
        if (eq == std::string::npos) {
            Log(LogLevel::Warn, "Ignore invalid line " + std::to_string(lineNo) + ": " + t);
            continue;
        }
        const std::string k = Trim(t.substr(0, eq));
        const std::string v = Trim(t.substr(eq + 1));
        kv[k] = v;
    }

    auto get = [&](const char* key) -> std::string {
        auto it = kv.find(key);
        if (it == kv.end()) {
            return "";
        }
        return it->second;
    };

    out->rendezvous_addr = get("rendezvous_addr");
    if (!get("rendezvous_port").empty()
        && (!ParseUInt16(get("rendezvous_port"), &out->rendezvous_port)
            || out->rendezvous_port == 0)) {
        Log(LogLevel::Error, "Invalid rendezvous_port");
        return false;
    }
    out->room_id = get("room_id");
    out->peer_id = get("peer_id");
    out->target_peer_id = get("target_peer_id");
    out->auth_token = get("auth_token");
    if (!get("keepalive_interval").empty()
        && (!ParseUInt16(get("keepalive_interval"), &out->keepalive_interval)
            || out->keepalive_interval == 0)) {
        Log(LogLevel::Error, "Invalid keepalive_interval");
        return false;
    }
    if (!get("peer_timeout").empty()
        && (!ParseUInt16(get("peer_timeout"), &out->peer_timeout)
            || out->peer_timeout <= out->keepalive_interval)) {
        Log(LogLevel::Error, "peer_timeout must be greater than keepalive_interval");
        return false;
    }
    if (!get("dummy_traffic_enabled").empty()) {
        out->dummy_traffic_enabled = ParseBool(get("dummy_traffic_enabled"));
    }
    if (!get("punch_timeout").empty()
        && (!ParseUInt16(get("punch_timeout"), &out->punch_timeout)
            || out->punch_timeout == 0)) {
        Log(LogLevel::Error, "Invalid punch_timeout");
        return false;
    }
    if (get("traversal_modes").empty()) {
        Log(LogLevel::Error, "Missing required config key: traversal_modes");
        return false;
    }
    std::string traversalModesError;
    if (!ParseTraversalModes(get("traversal_modes"), &out->traversal_modes,
                             &traversalModesError)) {
        Log(LogLevel::Error, "Invalid traversal_modes: " + traversalModesError);
        return false;
    }
    if (std::none_of(out->traversal_modes.begin(), out->traversal_modes.end(),
                     [](const auto& mode) { return mode.enabled; })) {
        Log(LogLevel::Error, "At least one traversal mode must be enabled");
        return false;
    }
    if (!get("nat4_source_port_start").empty()
        && (!ParseUInt16(get("nat4_source_port_start"), &out->nat4_source_port_start)
            || out->nat4_source_port_start == 0)) {
        Log(LogLevel::Error, "Invalid nat4_source_port_start");
        return false;
    }
    if (!get("nat4_source_port_count").empty()
        && (!ParseUInt16(get("nat4_source_port_count"), &out->nat4_source_port_count)
            || out->nat4_source_port_count == 0
            || out->nat4_source_port_count > 60)) {
        Log(LogLevel::Error, "nat4_source_port_count must be between 1 and 60");
        return false;
    }
    if (!get("nat4_peer_port_offset").empty()
        && (!ParseUInt16(get("nat4_peer_port_offset"), &out->nat4_peer_port_offset)
            || out->nat4_peer_port_offset > 256)) {
        Log(LogLevel::Error, "nat4_peer_port_offset must be between 0 and 256");
        return false;
    }
    if (!get("nat4_round_timeout").empty()
        && (!ParseUInt16(get("nat4_round_timeout"), &out->nat4_round_timeout)
            || out->nat4_round_timeout == 0 || out->nat4_round_timeout > 60)) {
        Log(LogLevel::Error, "nat4_round_timeout must be between 1 and 60");
        return false;
    }
    if (!get("ipv6_accept_inbound").empty()) {
        out->ipv6_accept_inbound = ParseBool(get("ipv6_accept_inbound"));
    }
    if (!get("ipv6_listen_port").empty()
        && !ParseUInt16(get("ipv6_listen_port"), &out->ipv6_listen_port)) {
        Log(LogLevel::Error, "Invalid ipv6_listen_port");
        return false;
    }
    if (!get("ipv6_probe_host").empty()) {
        out->ipv6_probe_host = get("ipv6_probe_host");
    }
    if (!get("ipv6_probe_port").empty()
        && (!ParseUInt16(get("ipv6_probe_port"), &out->ipv6_probe_port)
            || out->ipv6_probe_port == 0)) {
        Log(LogLevel::Error, "Invalid ipv6_probe_port");
        return false;
    }
    if (!get("ipv6_fallback_timeout").empty()
        && (!ParseUInt16(get("ipv6_fallback_timeout"), &out->ipv6_fallback_timeout)
            || out->ipv6_fallback_timeout == 0
            || out->ipv6_fallback_timeout > 120)) {
        Log(LogLevel::Error, "ipv6_fallback_timeout must be between 1 and 120");
        return false;
    }
    if (out->nat4_source_port_count > 0
        && static_cast<uint32_t>(out->nat4_source_port_start)
            + out->nat4_source_port_count - 1 > 65535) {
        Log(LogLevel::Error, "NAT4 source port range exceeds 65535");
        return false;
    }
    if (!get("adapter_name").empty()) {
        out->adapter_name = get("adapter_name");
    }
    out->local_tun_ipv4 = get("local_tun_ipv4");
    if (!get("tun_prefix").empty()) {
        if (!ParseUInt8(get("tun_prefix"), &out->tun_prefix)) {
            Log(LogLevel::Error, "Invalid tun_prefix: " + get("tun_prefix") + ", must be 0..32");
            return false;
        }
    }
    if (!get("tun_mtu").empty()) {
        if (!ParseUInt16(get("tun_mtu"), &out->tun_mtu)) {
            Log(LogLevel::Error, "Invalid tun_mtu: " + get("tun_mtu") + ", must be 576..9000");
            return false;
        }
    }
    if (!get("auto_config_ipv4").empty()) {
        out->auto_config_ipv4 = ParseBool(get("auto_config_ipv4"));
    }
    if (!get("log_level").empty()) {
        if (!TryParseLogLevel(get("log_level"), &out->log_level)) {
            Log(LogLevel::Error, "Invalid log_level: " + get("log_level") + ", must be one of: Debug, Info, Warn, Error");
            return false;
        }
    }

    if (out->local_tun_ipv4.empty()) {
        Log(LogLevel::Error, "Missing required config key: local_tun_ipv4");
        return false;
    }
    if (out->rendezvous_addr.empty() || out->room_id.empty() || out->peer_id.empty()) {
        Log(LogLevel::Error, "Missing rendezvous_addr, room_id or peer_id");
        return false;
    }
    if (out->tun_prefix > 32) {
        Log(LogLevel::Error, "Invalid tun_prefix, must be 0..32");
        return false;
    }
    if (out->tun_mtu < 576 || out->tun_mtu > 9000) {
        Log(LogLevel::Error, "Invalid tun_mtu, must be 576..9000");
        return false;
    }
    if (out->tun_mtu > 1472) {
        Log(LogLevel::Warn, "tun_mtu is greater than 1472; may cause UDP/IPv4 fragmentation");
    }
    if (out->ipv6_probe_host.empty()
        || out->ipv6_probe_host.size() > 255
        || out->ipv6_probe_host.find_first_of("\t\r\n") != std::string::npos) {
        Log(LogLevel::Error, "Invalid ipv6_probe_host");
        return false;
    }
    return true;
}
