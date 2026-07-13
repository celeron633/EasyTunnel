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
    uint16_t punch_timeout = 30;
    uint16_t nat4_max_port_offset = 20;
    std::string adapter_name = "EasyTunnel";
    std::string local_tun_ipv4;
    uint8_t tun_prefix = 24;
    uint16_t tun_mtu = 1452;
    bool auto_config_ipv4 = true;
    LogLevel log_level = LogLevel::Info;
};

bool LoadConfig(const std::string& file, Config* out);
