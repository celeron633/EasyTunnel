#pragma once

#include <cstdint>
#include <string>

struct Config {
    std::string local_ipv6;
    std::string peer_ipv6;
    uint16_t udp_port = 44556;
    std::string adapter_name = "6Tunnel";
    std::string tunnel_type = "6Tunnel";
    std::string local_tun_ipv4;
    uint8_t tun_prefix = 24;
    bool auto_config_ipv4 = true;
    bool verbose_debug = true;
};

bool LoadConfig(const std::string& file, Config* out);
