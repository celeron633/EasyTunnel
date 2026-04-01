#include "config.h"

#include <fstream>
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
        out->udp_port = static_cast<uint16_t>(std::stoi(get("udp_port")));
    }
    if (!get("adapter_name").empty()) {
        out->adapter_name = get("adapter_name");
    }
    if (!get("tunnel_type").empty()) {
        out->tunnel_type = get("tunnel_type");
    }
    out->local_tun_ipv4 = get("local_tun_ipv4");
    if (!get("tun_prefix").empty()) {
        out->tun_prefix = static_cast<uint8_t>(std::stoi(get("tun_prefix")));
    }
    if (!get("auto_config_ipv4").empty()) {
        out->auto_config_ipv4 = ParseBool(get("auto_config_ipv4"));
    }
    if (!get("verbose_debug").empty()) {
        out->verbose_debug = ParseBool(get("verbose_debug"));
    }

    if (out->peer_ipv6.empty() || out->local_tun_ipv4.empty()) {
        Log(LogLevel::Error, "Missing required config keys: peer_ipv6 / local_tun_ipv4");
        return false;
    }
    if (out->tun_prefix > 32) {
        Log(LogLevel::Error, "Invalid tun_prefix, must be 0..32");
        return false;
    }
    return true;
}
