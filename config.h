#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "log.h"

enum class TraversalMode {
    NatPunch,
    Ipv6,
    Ipv4Relay,
};

struct TraversalModeSetting {
    TraversalMode mode;
    bool enabled;
};

struct StunServerConfig {
    std::string host;
    uint16_t port = 3478;
};

struct Config;

std::vector<TraversalModeSetting> DefaultTraversalModes();
const char* TraversalModeName(TraversalMode mode);
const char* TraversalModeDisplayName(TraversalMode mode);
bool ParseTraversalModes(const std::string& text,
                         std::vector<TraversalModeSetting>* modes,
                         std::string* error);
std::string SerializeTraversalModes(
    const std::vector<TraversalModeSetting>& modes);
bool IsTraversalModeEnabled(const Config& config, TraversalMode mode);
std::vector<TraversalMode> EnabledTraversalModes(
    const std::vector<TraversalModeSetting>& modes);
std::string SerializeTraversalModeSequence(
    const std::vector<TraversalMode>& modes);
bool ParseTraversalModeSequence(const std::string& text,
                                std::vector<TraversalMode>* modes,
                                std::string* error);
std::vector<TraversalMode> IntersectTraversalModes(
    const std::vector<TraversalMode>& preferred,
    const std::vector<TraversalMode>& supported);

struct Config {
    std::string rendezvous_addr;
    uint16_t rendezvous_port = 3478;
    std::string room_id;
    std::string peer_id;
    std::string target_peer_id;
    std::string auth_token;
    uint16_t keepalive_interval = 15;
    uint16_t peer_timeout = 45;
    bool dummy_traffic_enabled = false;
    uint16_t punch_timeout = 30;
    uint16_t nat_punch_attempt_limit = 3;
    std::vector<StunServerConfig> stun_servers;
    std::vector<TraversalModeSetting> traversal_modes = DefaultTraversalModes();
    bool ipv6_accept_inbound = false;
    uint16_t ipv6_listen_port = 0;
    std::string ipv6_probe_host = "2400:3200::1";
    uint16_t ipv6_probe_port = 53;
    uint16_t ipv6_fallback_timeout = 15;
    std::string adapter_name = "EasyTunnel";
    std::string local_tun_ipv4;
    uint8_t tun_prefix = 24;
    uint16_t tun_mtu = 1452;
    bool auto_config_ipv4 = true;
    LogLevel log_level = LogLevel::Info;
};
