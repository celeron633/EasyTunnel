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
        const size_t idx = 0;
        (void)idx;
        const unsigned long v = std::stoul(text);
        if (v > std::numeric_limits<uint16_t>::max()) {
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
        const unsigned long v = std::stoul(text);
        if (v > std::numeric_limits<uint8_t>::max()) {
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

    out->local_ipv6 = get("local_ipv6");
    if (out->local_ipv6.empty()) {
        out->local_ipv6 = "::";
    }
    out->peer_ipv6 = get("peer_ipv6");
    if (!get("udp_port").empty()) {
        if (!ParseUInt16(get("udp_port"), &out->udp_port) || out->udp_port == 0) {
            Log(LogLevel::Error, "Invalid udp_port: " + get("udp_port") + ", must be 1..65535");
            return false;
        }
    }
    if (!get("adapter_name").empty()) {
        out->adapter_name = get("adapter_name");
    }
    if (!get("tunnel_type").empty()) {
        out->tunnel_type = get("tunnel_type");
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

    if (out->peer_ipv6.empty() || out->local_tun_ipv4.empty()) {
        Log(LogLevel::Error, "Missing required config keys: peer_ipv6 / local_tun_ipv4");
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
    if (out->tun_mtu > 1452) {
        Log(LogLevel::Warn, "tun_mtu is greater than 1452; may cause IPv6/UDP outer fragmentation on 1500-byte paths");
    }
    return true;
}
