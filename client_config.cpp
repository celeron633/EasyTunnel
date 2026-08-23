#include "client_config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <utility>

#include <json/json.h>

#include "nat_protocol.h"
#include "util.h"

namespace {
// Mirrors the previous hand-rolled parser's leniency: a missing key or a key
// whose JSON type does not match keeps the struct's current value instead of
// failing the whole file.
void ReadString(const Json::Value& root, const char* key, std::string* value) {
    if (root.isMember(key) && root[key].isString()) *value = root[key].asString();
}

void ReadInt(const Json::Value& root, const char* key, int* value) {
    if (root.isMember(key) && root[key].isInt()) *value = root[key].asInt();
}

void ReadBool(const Json::Value& root, const char* key, bool* value) {
    if (root.isMember(key) && root[key].isBool()) *value = root[key].asBool();
}

bool ReadStunServers(const Json::Value& root,
                     std::vector<StunServerConfig>* servers,
                     std::string* error) {
    if (!root.isMember("stun_servers")) return true;
    const Json::Value& value = root["stun_servers"];
    if (!value.isArray()) {
        *error = "stun_servers must be an array";
        return false;
    }

    std::vector<StunServerConfig> parsed;
    for (const auto& item : value) {
        if (!item.isObject() || !item.isMember("host")
            || !item["host"].isString() || item["host"].asString().empty()
            || !item.isMember("port") || !item["port"].isInt()) {
            *error = "Each STUN server must contain host and port";
            return false;
        }
        const int port = item["port"].asInt();
        if (port < 1 || port > 65535) {
            *error = "STUN server port must be between 1 and 65535";
            return false;
        }
        parsed.push_back({item["host"].asString(),
                          static_cast<uint16_t>(port)});
    }
    if (parsed.size() != 2) {
        *error = "Exactly two STUN servers are required";
        return false;
    }
    *servers = std::move(parsed);
    return true;
}

bool IsValidStunHost(const std::string& host) {
    return !host.empty() && host.size() <= 253
        && std::none_of(host.begin(), host.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        });
}
}  // namespace

bool LoadClientConfig(const std::string& path, ClientConfig* config,
                      bool* existed, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        *existed = false;
        return true;
    }
    *existed = true;

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string parseErrors;
    if (!Json::parseFromStream(builder, input, &root, &parseErrors)
        || !root.isObject()) {
        *error = "Invalid JSON object: " + path;
        return false;
    }

    ReadString(root, "rendezvous_address", &config->rendezvousAddress);
    ReadInt(root, "rendezvous_port", &config->rendezvousPort);
    ReadString(root, "room_id", &config->roomId);
    ReadString(root, "peer_id", &config->peerId);
    ReadString(root, "auth_token", &config->authToken);
    ReadString(root, "adapter_name", &config->adapterName);
    ReadString(root, "local_tun_ipv4", &config->localTunIpv4);
    ReadInt(root, "tun_prefix", &config->tunPrefix);
    ReadInt(root, "tun_mtu", &config->tunMtu);
    ReadBool(root, "auto_config_ipv4", &config->autoConfigIpv4);
    ReadInt(root, "keepalive_interval", &config->keepaliveInterval);
    ReadInt(root, "peer_timeout", &config->peerTimeout);
    ReadBool(root, "dummy_traffic_enabled", &config->dummyTrafficEnabled);
    ReadInt(root, "punch_timeout", &config->punchTimeout);
    if (!ReadStunServers(root, &config->stunServers, error)) return false;
    if (root.isMember("traversal_modes") && root["traversal_modes"].isString()
        && !ParseTraversalModes(root["traversal_modes"].asString(),
                                &config->traversalModes, error)) {
        *error = "Invalid traversal_modes: " + *error;
        return false;
    }
    ReadBool(root, "ipv6_accept_inbound", &config->ipv6AcceptInbound);
    ReadInt(root, "ipv6_listen_port", &config->ipv6ListenPort);
    ReadString(root, "ipv6_probe_host", &config->ipv6ProbeHost);
    ReadInt(root, "ipv6_probe_port", &config->ipv6ProbePort);
    ReadInt(root, "ipv6_fallback_timeout", &config->ipv6FallbackTimeout);
    ReadInt(root, "log_level", &config->logLevel);
    ReadInt(root, "rendezvous_retry_delay_seconds",
            &config->rendezvousRetryDelaySeconds);
    ReadBool(root, "auto_wait_for_peer", &config->autoWaitForPeer);
    ReadBool(root, "close_to_minimize", &config->closeToMinimize);

    config->rendezvousPort = std::clamp(config->rendezvousPort, 1, 65535);
    config->tunPrefix = std::clamp(config->tunPrefix, 0, 32);
    config->tunMtu = std::clamp(config->tunMtu, 576, 9000);
    config->keepaliveInterval = std::clamp(config->keepaliveInterval, 1, 300);
    config->peerTimeout = std::clamp(config->peerTimeout,
                                     config->keepaliveInterval + 1, 3600);
    config->punchTimeout = std::clamp(config->punchTimeout, 1, 600);
    config->ipv6ListenPort = std::clamp(config->ipv6ListenPort, 0, 65535);
    config->ipv6ProbePort = std::clamp(config->ipv6ProbePort, 1, 65535);
    config->ipv6FallbackTimeout = std::clamp(config->ipv6FallbackTimeout, 1, 120);
    config->logLevel = std::clamp(config->logLevel, 0, 3);
    config->rendezvousRetryDelaySeconds = std::clamp(
        config->rendezvousRetryDelaySeconds, 1, 3600);
    return true;
}

bool SaveClientConfig(const std::string& path, const ClientConfig& config,
                      std::string* error) {
    Json::Value root;
    root["rendezvous_address"] = config.rendezvousAddress;
    root["rendezvous_port"] = config.rendezvousPort;
    root["room_id"] = config.roomId;
    root["peer_id"] = config.peerId;
    root["auth_token"] = config.authToken;
    root["adapter_name"] = config.adapterName;
    root["local_tun_ipv4"] = config.localTunIpv4;
    root["tun_prefix"] = config.tunPrefix;
    root["tun_mtu"] = config.tunMtu;
    root["auto_config_ipv4"] = config.autoConfigIpv4;
    root["keepalive_interval"] = config.keepaliveInterval;
    root["peer_timeout"] = config.peerTimeout;
    root["dummy_traffic_enabled"] = config.dummyTrafficEnabled;
    root["punch_timeout"] = config.punchTimeout;
    Json::Value stunServers(Json::arrayValue);
    for (const auto& server : config.stunServers) {
        Json::Value item(Json::objectValue);
        item["host"] = server.host;
        item["port"] = server.port;
        stunServers.append(std::move(item));
    }
    root["stun_servers"] = std::move(stunServers);
    root["traversal_modes"] = SerializeTraversalModes(config.traversalModes);
    root["ipv6_accept_inbound"] = config.ipv6AcceptInbound;
    root["ipv6_listen_port"] = config.ipv6ListenPort;
    root["ipv6_probe_host"] = config.ipv6ProbeHost;
    root["ipv6_probe_port"] = config.ipv6ProbePort;
    root["ipv6_fallback_timeout"] = config.ipv6FallbackTimeout;
    root["log_level"] = config.logLevel;
    root["rendezvous_retry_delay_seconds"] = config.rendezvousRetryDelaySeconds;
    root["auto_wait_for_peer"] = config.autoWaitForPeer;
    root["close_to_minimize"] = config.closeToMinimize;

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        *error = "Cannot save configuration: " + path;
        return false;
    }
    Json::StreamWriterBuilder writerBuilder;
    writerBuilder["indentation"] = "  ";
    output << Json::writeString(writerBuilder, root) << '\n';
    output.flush();
    if (!output.good()) {
        *error = "Cannot write configuration: " + path;
        return false;
    }
    return true;
}

bool ValidateClientConfig(const ClientConfig& config, std::string* error) {
    if (config.rendezvousAddress.empty()) {
        *error = "Rendezvous server is required";
        return false;
    }
    if (!IsSafeControlField(config.roomId)) { *error = "Invalid room ID"; return false; }
    if (!IsSafeControlField(config.peerId)) { *error = "Invalid peer ID"; return false; }
    if (!config.authToken.empty() && !IsSafeControlField(config.authToken)) {
        *error = "Invalid auth token";
        return false;
    }
    in_addr address{};
    if (!ParseIpv4(config.localTunIpv4, &address)) {
        *error = "Invalid local TUN IPv4";
        return false;
    }
    const bool anyModeEnabled = std::any_of(
        config.traversalModes.begin(), config.traversalModes.end(),
        [](const auto& setting) { return setting.enabled; });
    if (!anyModeEnabled) {
        *error = "Enable at least one traversal mode";
        return false;
    }
    const auto natPunchMode = std::find_if(
        config.traversalModes.begin(), config.traversalModes.end(),
        [](const auto& setting) {
            return setting.mode == TraversalMode::NatPunch;
        });
    if (natPunchMode != config.traversalModes.end()
        && natPunchMode->enabled) {
        if (config.stunServers.size() != 2
            || !IsValidStunHost(config.stunServers[0].host)
            || !IsValidStunHost(config.stunServers[1].host)) {
            *error = "Adaptive NAT Punch requires two valid STUN servers";
            return false;
        }
        if (config.stunServers[0].host == config.stunServers[1].host
            && config.stunServers[0].port == config.stunServers[1].port) {
            *error = "STUN A and STUN B must be different servers";
            return false;
        }
    }
    const auto ipv6Mode = std::find_if(
        config.traversalModes.begin(), config.traversalModes.end(),
        [](const auto& setting) { return setting.mode == TraversalMode::Ipv6; });
    if (ipv6Mode != config.traversalModes.end() && ipv6Mode->enabled
        && config.ipv6ProbeHost.empty()) {
        *error = "IPv6 probe host is required";
        return false;
    }
    return true;
}

// The clamps mirror LoadClientConfig, so a config that came through the loader
// converts without further checks; UI-edited values are clamped here again.
Config ToEngineConfig(const ClientConfig& config,
                      const std::string& targetPeerId) {
    Config output;
    output.rendezvous_addr = config.rendezvousAddress;
    output.rendezvous_port = static_cast<uint16_t>(
        std::clamp(config.rendezvousPort, 1, 65535));
    output.room_id = config.roomId;
    output.peer_id = config.peerId;
    output.target_peer_id = targetPeerId;
    output.auth_token = config.authToken;
    output.adapter_name = config.adapterName;
    output.local_tun_ipv4 = config.localTunIpv4;
    output.tun_prefix = static_cast<uint8_t>(std::clamp(config.tunPrefix, 0, 32));
    output.tun_mtu = static_cast<uint16_t>(std::clamp(config.tunMtu, 576, 9000));
    output.auto_config_ipv4 = config.autoConfigIpv4;
    output.keepalive_interval = static_cast<uint16_t>(
        std::clamp(config.keepaliveInterval, 1, 300));
    output.peer_timeout = static_cast<uint16_t>(
        std::clamp(config.peerTimeout, output.keepalive_interval + 1, 3600));
    output.dummy_traffic_enabled = config.dummyTrafficEnabled;
    output.punch_timeout = static_cast<uint16_t>(
        std::clamp(config.punchTimeout, 1, 600));
    output.stun_servers = config.stunServers;
    output.traversal_modes = config.traversalModes;
    output.ipv6_accept_inbound = config.ipv6AcceptInbound;
    output.ipv6_listen_port = static_cast<uint16_t>(
        std::clamp(config.ipv6ListenPort, 0, 65535));
    output.ipv6_probe_host = config.ipv6ProbeHost;
    output.ipv6_probe_port = static_cast<uint16_t>(
        std::clamp(config.ipv6ProbePort, 1, 65535));
    output.ipv6_fallback_timeout = static_cast<uint16_t>(
        std::clamp(config.ipv6FallbackTimeout, 1, 120));
    output.log_level = static_cast<LogLevel>(std::clamp(config.logLevel, 0, 3));
    return output;
}
