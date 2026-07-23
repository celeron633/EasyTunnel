#pragma once

#include <cstdint>
#include <string>

#include "log.h"

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
    uint16_t nat4_source_port_start = 30000;
    uint16_t nat4_source_port_count = 25;
    uint16_t nat4_peer_port_offset = 20;
    uint16_t nat4_round_timeout = 10;
    bool ipv6_fallback_enabled = false;
    bool ipv6_accept_inbound = false;
    uint16_t ipv6_listen_port = 0;
    std::string ipv6_probe_host = "2400:3200::1";
    uint16_t ipv6_probe_port = 53;
    uint16_t ipv6_fallback_timeout = 15;
    bool ipv4_relay_fallback_enabled = false;
    std::string adapter_name = "EasyTunnel";
    std::string local_tun_ipv4;
    uint8_t tun_prefix = 24;
    uint16_t tun_mtu = 1452;
    bool auto_config_ipv4 = true;
    LogLevel log_level = LogLevel::Info;
};

bool LoadConfig(const std::string& file, Config* out);
