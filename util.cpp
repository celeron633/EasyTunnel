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

#ifdef _WIN32

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

bool DisableTunIpv6(const Config& cfg) {
    // Use adapter binding toggle for reliable suppression of IPv6 noise on this tunnel NIC.
    // This is reversible. Restore command example:
    //   Enable-NetAdapterBinding -Name "<adapter>" -ComponentID ms_tcpip6
    std::ostringstream ps;
    ps << "powershell -NoProfile -ExecutionPolicy Bypass -Command \""
       << "$ErrorActionPreference='Stop';"
       << "$name='" << cfg.adapter_name << "';"
       << "$b=Get-NetAdapterBinding -Name $name -ComponentID ms_tcpip6 -ErrorAction Stop;"
       << "if($b.Enabled){Disable-NetAdapterBinding -Name $name -ComponentID ms_tcpip6 -Confirm:$false -ErrorAction Stop | Out-Null}"
       << "\"";

    if (!RunCommand(ps.str())) {
        return false;
    }

    Log(LogLevel::Info,
        "IPv6 binding disabled on adapter. To restore later: Enable-NetAdapterBinding -Name \""
        + cfg.adapter_name + "\" -ComponentID ms_tcpip6");

    return true;
}

bool ParseIpv6(const std::string& ip, in6_addr* out) {
    return InetPtonA(AF_INET6, ip.c_str(), out) == 1;
}

#else  // Linux / POSIX

bool ConfigureTunIpv4(const Config& cfg) {
    // Assign the IPv4 address using CIDR notation.
    std::ostringstream ss;
    ss << "ip addr add " << cfg.local_tun_ipv4
       << "/" << static_cast<int>(cfg.tun_prefix)
       << " dev " << cfg.adapter_name;
    if (!RunCommand(ss.str())) {
        return false;
    }
    // Bring the interface up.
    std::ostringstream up;
    up << "ip link set " << cfg.adapter_name << " up";
    return RunCommand(up.str());
}

bool ConfigureTunMtu(const Config& cfg) {
    std::ostringstream ss;
    ss << "ip link set " << cfg.adapter_name << " mtu " << cfg.tun_mtu;
    return RunCommand(ss.str());
}

bool ParseIpv6(const std::string& ip, in6_addr* out) {
    return inet_pton(AF_INET6, ip.c_str(), out) == 1;
}

#endif  // _WIN32

std::string IpProtoToName(uint8_t proto) {
    switch (proto) {
        case 6:
            return "TCP";
        case 17:
            return "UDP";
        case 58:
            return "ICMPv6";
        case 41:
            return "IPv6";
        case 47:
            return "GRE";
        case 50:
            return "ESP";
        case 51:
            return "AH";
        default:
            return "Proto-" + std::to_string(static_cast<unsigned>(proto));
    }
}

std::string NonIpv4PacketType(const uint8_t* packet, size_t len) {
    if (packet == nullptr || len == 0) {
        return "Empty";
    }
    const uint8_t version = (packet[0] >> 4U) & 0x0FU;
    if (version == 6) {
        if (len >= 40) {
            const uint8_t nextHeader = packet[6];
            return "IPv6/" + IpProtoToName(nextHeader);
        }
        return "IPv6";
    }
    if (version == 4) {
        return "IPv4";
    }
    return "Unknown(v=" + std::to_string(static_cast<unsigned>(version)) + ")";
}

std::string Ipv4ProtocolToString(const uint8_t* packet, size_t len) {
	if (packet == nullptr || len < 20) {
		return "Unknown";
	}
	const uint8_t protocol = packet[9];
	switch (protocol) {
		case 1:
			return "ICMP";
		case 2:
			return "IGMP";
		case 4:
			return "IP-in-IP";
		case 6:
			return "TCP";
		case 17:
			return "UDP";
        case 41:
        case 47:
        case 50:
        case 51:
		case 58:
            return IpProtoToName(protocol);
		case 89:
			return "OSPF";
		case 112:
			return "VRRP";
		case 132:
			return "SCTP";
		default:
			return "Proto-" + std::to_string(static_cast<unsigned>(protocol));
	}
}
