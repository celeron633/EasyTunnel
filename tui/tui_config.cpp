#include "tui_config.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace {
std::string Escape(const std::string& value) {
    std::string output;
    for (const char c : value) {
        switch (c) {
            case '\\': output += "\\\\"; break;
            case '"': output += "\\\""; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default: output += c; break;
        }
    }
    return output;
}

size_t ValueStart(const std::string& json, const std::string& key) {
    const size_t keyPosition = json.find("\"" + key + "\"");
    if (keyPosition == std::string::npos) return std::string::npos;
    const size_t colon = json.find(':', keyPosition + key.size() + 2);
    if (colon == std::string::npos) return std::string::npos;
    return json.find_first_not_of(" \t\r\n", colon + 1);
}

bool StringValue(const std::string& json, const std::string& key, std::string* value) {
    size_t position = ValueStart(json, key);
    if (position == std::string::npos || json[position] != '"') return false;
    ++position;
    std::string output;
    bool escaped = false;
    for (; position < json.size(); ++position) {
        const char c = json[position];
        if (escaped) {
            switch (c) {
                case 'n': output += '\n'; break;
                case 'r': output += '\r'; break;
                case 't': output += '\t'; break;
                case '\\': output += '\\'; break;
                case '"': output += '"'; break;
                default: return false;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            *value = std::move(output);
            return true;
        } else {
            output += c;
        }
    }
    return false;
}

bool IntValue(const std::string& json, const std::string& key, int* value) {
    const size_t position = ValueStart(json, key);
    if (position == std::string::npos) return false;
    try {
        size_t consumed = 0;
        const int parsed = std::stoi(json.substr(position), &consumed);
        if (consumed == 0) return false;
        *value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool BoolValue(const std::string& json, const std::string& key, bool* value) {
    const size_t position = ValueStart(json, key);
    if (position == std::string::npos) return false;
    if (json.compare(position, 4, "true") == 0) { *value = true; return true; }
    if (json.compare(position, 5, "false") == 0) { *value = false; return true; }
    return false;
}
}  // namespace

bool LoadTuiConfig(const std::string& path, TuiConfig* config,
                   bool* existed, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        *existed = false;
        return true;
    }
    *existed = true;
    std::ostringstream contents;
    contents << input.rdbuf();
    const std::string json = contents.str();
    if (json.find('{') == std::string::npos || json.find('}') == std::string::npos) {
        *error = "Invalid JSON object: " + path;
        return false;
    }
    StringValue(json, "rendezvous_address", &config->rendezvousAddress);
    IntValue(json, "rendezvous_port", &config->rendezvousPort);
    StringValue(json, "room_id", &config->roomId);
    StringValue(json, "peer_id", &config->peerId);
    StringValue(json, "auth_token", &config->authToken);
    StringValue(json, "adapter_name", &config->adapterName);
    StringValue(json, "local_tun_ipv4", &config->localTunIpv4);
    IntValue(json, "tun_prefix", &config->tunPrefix);
    IntValue(json, "tun_mtu", &config->tunMtu);
    BoolValue(json, "auto_config_ipv4", &config->autoConfigIpv4);
    IntValue(json, "keepalive_interval", &config->keepaliveInterval);
    IntValue(json, "peer_timeout", &config->peerTimeout);
    IntValue(json, "punch_timeout", &config->punchTimeout);
    IntValue(json, "nat4_source_port_start", &config->nat4SourcePortStart);
    IntValue(json, "nat4_source_port_count", &config->nat4SourcePortCount);
    IntValue(json, "nat4_peer_port_offset", &config->nat4PeerPortOffset);
    IntValue(json, "nat4_round_timeout", &config->nat4RoundTimeout);
    IntValue(json, "log_level", &config->logLevel);
    BoolValue(json, "auto_wait_for_peer", &config->autoWaitForPeer);

    config->rendezvousPort = std::clamp(config->rendezvousPort, 1, 65535);
    config->tunPrefix = std::clamp(config->tunPrefix, 0, 32);
    config->tunMtu = std::clamp(config->tunMtu, 576, 9000);
    config->keepaliveInterval = std::clamp(config->keepaliveInterval, 1, 300);
    config->peerTimeout = std::clamp(config->peerTimeout,
                                     config->keepaliveInterval + 1, 3600);
    config->punchTimeout = std::clamp(config->punchTimeout, 1, 600);
    config->nat4SourcePortStart = std::clamp(config->nat4SourcePortStart, 1, 65535);
    config->nat4SourcePortCount = std::clamp(config->nat4SourcePortCount, 0, 60);
    if (config->nat4SourcePortCount > 0) {
        config->nat4SourcePortStart = (std::min)(
            config->nat4SourcePortStart, 65536 - config->nat4SourcePortCount);
    }
    config->nat4PeerPortOffset = std::clamp(config->nat4PeerPortOffset, 0, 256);
    config->nat4RoundTimeout = std::clamp(config->nat4RoundTimeout, 1, 60);
    config->logLevel = std::clamp(config->logLevel, 0, 3);
    return true;
}

bool SaveTuiConfig(const std::string& path, const TuiConfig& config,
                   std::string* error) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        *error = "Cannot save TUI configuration: " + path;
        return false;
    }
    output
        << "{\n"
        << "  \"rendezvous_address\": \"" << Escape(config.rendezvousAddress) << "\",\n"
        << "  \"rendezvous_port\": " << config.rendezvousPort << ",\n"
        << "  \"room_id\": \"" << Escape(config.roomId) << "\",\n"
        << "  \"peer_id\": \"" << Escape(config.peerId) << "\",\n"
        << "  \"auth_token\": \"" << Escape(config.authToken) << "\",\n"
        << "  \"adapter_name\": \"" << Escape(config.adapterName) << "\",\n"
        << "  \"local_tun_ipv4\": \"" << Escape(config.localTunIpv4) << "\",\n"
        << "  \"tun_prefix\": " << config.tunPrefix << ",\n"
        << "  \"tun_mtu\": " << config.tunMtu << ",\n"
        << "  \"auto_config_ipv4\": " << (config.autoConfigIpv4 ? "true" : "false") << ",\n"
        << "  \"keepalive_interval\": " << config.keepaliveInterval << ",\n"
        << "  \"peer_timeout\": " << config.peerTimeout << ",\n"
        << "  \"punch_timeout\": " << config.punchTimeout << ",\n"
        << "  \"nat4_source_port_start\": " << config.nat4SourcePortStart << ",\n"
        << "  \"nat4_source_port_count\": " << config.nat4SourcePortCount << ",\n"
        << "  \"nat4_peer_port_offset\": " << config.nat4PeerPortOffset << ",\n"
        << "  \"nat4_round_timeout\": " << config.nat4RoundTimeout << ",\n"
        << "  \"log_level\": " << config.logLevel << ",\n"
        << "  \"auto_wait_for_peer\": " << (config.autoWaitForPeer ? "true" : "false") << "\n"
        << "}\n";
    output.flush();
    if (!output.good()) {
        *error = "Cannot write TUI configuration: " + path;
        return false;
    }
    return true;
}
