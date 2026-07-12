#pragma once

#include <cstdint>
#include <string>

#include "log.h"

struct Config {
    std::string local_addr;
    std::string peer_addr;
    uint16_t udp_port = 44556;
    std::string adapter_name = "EasyTunnel";
    std::string tunnel_type = "EasyTunnel";
    std::string local_tun_ipv4;
    uint8_t tun_prefix = 24;
    uint16_t tun_mtu = 1452;
    bool auto_config_ipv4 = true;
    LogLevel log_level = LogLevel::Info;
};

bool LoadConfig(const std::string& file, Config* out);
