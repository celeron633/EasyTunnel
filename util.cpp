#include "util.h"

#include <cstdlib>
#include <sstream>

#include "log.h"

bool IsIpv4Packet(const uint8_t* data, size_t len) {
    if (len < 20) {
        return false;
    }
    const uint8_t version = (data[0] >> 4U) & 0x0FU;
    return version == 4;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        return std::wstring();
    }
    std::wstring w(size - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], size);
    return w;
}

std::string PrefixToMask(uint8_t prefix) {
    uint32_t mask = 0;
    if (prefix == 0) {
        mask = 0;
    } else {
        mask = (0xFFFFFFFFu << (32 - prefix));
    }
    std::ostringstream ss;
    ss << ((mask >> 24) & 0xFF) << '.'
       << ((mask >> 16) & 0xFF) << '.'
       << ((mask >> 8) & 0xFF) << '.'
       << (mask & 0xFF);
    return ss.str();
}

bool RunCommand(const std::string& cmd) {
    Log(LogLevel::Info, "Execute: " + cmd);
    const int code = std::system(cmd.c_str());
    if (code != 0) {
        Log(LogLevel::Error, "Command failed, exit code=" + std::to_string(code));
        return false;
    }
    return true;
}

bool ConfigureTunIpv4(const Config& cfg) {
    const std::string mask = PrefixToMask(cfg.tun_prefix);
    std::ostringstream ss;
    ss << "netsh interface ipv4 set address name=\""
       << cfg.adapter_name
       << "\" static "
       << cfg.local_tun_ipv4 << ' ' << mask;
    return RunCommand(ss.str());
}

bool ConfigureTunMtu(const Config& cfg) {
    std::ostringstream ss;
    ss << "netsh interface ipv4 set subinterface \""
       << cfg.adapter_name
       << "\" mtu=" << cfg.tun_mtu << " store=persistent";
    return RunCommand(ss.str());
}

bool ParseIpv6(const std::string& ip, in6_addr* out) {
    return InetPtonA(AF_INET6, ip.c_str(), out) == 1;
}
