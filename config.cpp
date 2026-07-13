#include "config.h"

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
    if (!get("nat4_source_port_start").empty()
        && (!ParseUInt16(get("nat4_source_port_start"), &out->nat4_source_port_start)
            || out->nat4_source_port_start == 0)) {
        Log(LogLevel::Error, "Invalid nat4_source_port_start");
        return false;
    }
    if (!get("nat4_source_port_count").empty()
        && (!ParseUInt16(get("nat4_source_port_count"), &out->nat4_source_port_count)
            || out->nat4_source_port_count > 60)) {
        Log(LogLevel::Error, "nat4_source_port_count must be between 0 and 60");
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
    return true;
}
