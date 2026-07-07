#include "util.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <thread>

#include "log.h"

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <sys/time.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Program control globals
// ---------------------------------------------------------------------------
std::atomic<bool> g_running{true};
std::atomic<bool> g_shutdownCompleted{false};

namespace {
std::atomic<int> g_stopSignalCount{0};
}  // namespace

// ---------------------------------------------------------------------------
// Signal / console-event handlers
// ---------------------------------------------------------------------------
#ifdef _WIN32

namespace {
constexpr int kForceExitTimeoutMs = 4000;

BOOL WINAPI ConsoleHandler(DWORD signal) {
    switch (signal) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT: {
            const int count = g_stopSignalCount.fetch_add(1) + 1;
            if (count == 1) {
                g_running.store(false);
                Log(LogLevel::Warn,
                    "Stop signal received, shutting down..."
                    " Press Ctrl+C again to force exit.");
                std::thread([]() {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(kForceExitTimeoutMs));
                    if (!g_shutdownCompleted.load()) {
                        Log(LogLevel::Error,
                            "Graceful shutdown timeout, force terminating process.");
                        TerminateProcess(GetCurrentProcess(), 130);
                    }
                }).detach();
            } else {
                Log(LogLevel::Error,
                    "Second stop signal received, force terminating immediately.");
                TerminateProcess(GetCurrentProcess(), 130);
            }
            return TRUE;
        }
        default:
            return FALSE;
    }
}
}  // namespace

#else  // POSIX

namespace {
void PosixSignalHandler(int /*sig*/) {
    const int count = g_stopSignalCount.fetch_add(1) + 1;
    if (count == 1) {
        g_running.store(false);
    } else {
        std::_Exit(130);
    }
}
}  // namespace

#endif  // _WIN32

void RegisterSignalHandlers() {
#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
#else
    struct sigaction sa {};
    sa.sa_handler = PosixSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif
}

// ---------------------------------------------------------------------------
// Cross-platform socket helpers
// ---------------------------------------------------------------------------

void SetSocketRecvTimeoutMs(socket_t sock, int timeoutMs) {
#ifdef _WIN32
    const DWORD ms = static_cast<DWORD>(timeoutMs);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

void ShutdownSocket(socket_t sock) {
    if (sock != kInvalidSocket) {
#ifdef _WIN32
        shutdown(sock, SD_BOTH);
#else
        shutdown(sock, SHUT_RDWR);
#endif
    }
}

void CloseSocket(socket_t& sock) {
    if (sock != kInvalidSocket) {
#ifdef _WIN32
        closesocket(sock);
#else
        ::close(sock);
#endif
        sock = kInvalidSocket;
    }
}

int GetSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool IsRecvTimeout(int err) {
#ifdef _WIN32
    return err == WSAETIMEDOUT || err == WSAEWOULDBLOCK || err == WSAEINTR;
#else
    return err == EAGAIN || err == EWOULDBLOCK || err == EINTR || err == ETIMEDOUT;
#endif
}

// ---------------------------------------------------------------------------
// Packet / protocol helpers
// ---------------------------------------------------------------------------

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

bool ParseIpv4(const std::string& ip, in_addr* out) {
    return InetPtonA(AF_INET, ip.c_str(), out) == 1;
}

#else  // Linux / POSIX

bool ConfigureTunIpv4(const Config& cfg) {
    // Use "replace" for idempotency: succeeds whether or not the address
    // is already assigned (avoids "RTNETLINK answers: File exists" on restart).
    std::ostringstream ss;
    ss << "ip addr replace " << cfg.local_tun_ipv4
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

bool ParseIpv4(const std::string& ip, in_addr* out) {
    return inet_pton(AF_INET, ip.c_str(), out) == 1;
}

#endif  // _WIN32

namespace {

std::string AnyAddressForFamily(int family) {
    return family == AF_INET ? "0.0.0.0" : "::";
}

std::string ConfiguredLocalAddress(const Config& cfg, int family) {
    if (cfg.local_addr.empty()) {
        return AnyAddressForFamily(family);
    }
    if (family == AF_INET && cfg.local_addr == "::") {
        return "0.0.0.0";
    }
    if (family == AF_INET6 && cfg.local_addr == "0.0.0.0") {
        return "::";
    }
    return cfg.local_addr;
}

const sockaddr* SockAddr(const UdpEndpoint& endpoint) {
    return reinterpret_cast<const sockaddr*>(&endpoint.addr);
}

}  // namespace

std::string AddressFamilyName(int family) {
    if (family == AF_INET) {
        return "IPv4";
    }
    if (family == AF_INET6) {
        return "IPv6";
    }
    return "unknown";
}

bool ParseUdpEndpoint(const std::string& ip, uint16_t port, UdpEndpoint* out) {
    if (out == nullptr) {
        return false;
    }

    sockaddr_in addr4{};
    addr4.sin_family = AF_INET;
    addr4.sin_port = htons(port);
    if (ParseIpv4(ip, &addr4.sin_addr)) {
        UdpEndpoint endpoint{};
        endpoint.family = AF_INET;
        endpoint.addr_len = static_cast<socket_len_t>(sizeof(addr4));
        std::memcpy(&endpoint.addr, &addr4, sizeof(addr4));
        *out = endpoint;
        return true;
    }

    sockaddr_in6 addr6{};
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons(port);
    if (ParseIpv6(ip, &addr6.sin6_addr)) {
        UdpEndpoint endpoint{};
        endpoint.family = AF_INET6;
        endpoint.addr_len = static_cast<socket_len_t>(sizeof(addr6));
        std::memcpy(&endpoint.addr, &addr6, sizeof(addr6));
        *out = endpoint;
        return true;
    }

    return false;
}

bool ValidateIpAddress(const std::string& ip) {
    UdpEndpoint endpoint{};
    return ParseUdpEndpoint(ip, 0, &endpoint);
}

bool OpenUdpSocket(const Config& cfg, int recvTimeoutMs, socket_t* sock,
                   UdpEndpoint* peer, std::string* localAddr,
                   std::string* peerAddr, std::string* error) {
    if (sock == nullptr || peer == nullptr) {
        if (error != nullptr) {
            *error = "Internal error: null socket output";
        }
        return false;
    }

    UdpEndpoint peerEndpoint{};
    if (!ParseUdpEndpoint(cfg.peer_addr, cfg.udp_port, &peerEndpoint)) {
        if (error != nullptr) {
            *error = "Invalid peer_addr: " + cfg.peer_addr;
        }
        return false;
    }

    std::string bindAddr = ConfiguredLocalAddress(cfg, peerEndpoint.family);
    UdpEndpoint localEndpoint{};
    if (!ParseUdpEndpoint(bindAddr, cfg.udp_port, &localEndpoint)
        || localEndpoint.family != peerEndpoint.family) {
        if (error != nullptr) {
            *error = "local_addr and peer_addr must both be IPv4 or both be IPv6";
        }
        return false;
    }

    socket_t opened = socket(peerEndpoint.family, SOCK_DGRAM, IPPROTO_UDP);
    if (opened == kInvalidSocket) {
        if (error != nullptr) {
            *error = "socket(" + AddressFamilyName(peerEndpoint.family)
                + ", UDP) failed. err=" + std::to_string(GetSocketError());
        }
        return false;
    }

    if (peerEndpoint.family == AF_INET6) {
        int v6only = 1;
        setsockopt(opened, IPPROTO_IPV6, IPV6_V6ONLY,
                   reinterpret_cast<const char*>(&v6only), sizeof(v6only));
    }
    SetSocketRecvTimeoutMs(opened, recvTimeoutMs);

    if (bind(opened, SockAddr(localEndpoint), localEndpoint.addr_len) != 0) {
        const int bindErr = GetSocketError();
        const std::string anyAddr = AnyAddressForFamily(peerEndpoint.family);
        if (bindAddr == anyAddr) {
            if (error != nullptr) {
                *error = "bind local_addr=" + bindAddr
                    + " failed. err=" + std::to_string(bindErr);
            }
            CloseSocket(opened);
            return false;
        }

        Log(LogLevel::Warn,
            "bind local_addr=" + bindAddr + " failed. err="
            + std::to_string(bindErr) + ", fallback to " + anyAddr);

        bindAddr = anyAddr;
        if (!ParseUdpEndpoint(bindAddr, cfg.udp_port, &localEndpoint)
            || bind(opened, SockAddr(localEndpoint), localEndpoint.addr_len) != 0) {
            if (error != nullptr) {
                *error = "bind fallback to " + anyAddr
                    + " failed. err=" + std::to_string(GetSocketError());
            }
            CloseSocket(opened);
            return false;
        }
    }

    *sock = opened;
    *peer = peerEndpoint;
    if (localAddr != nullptr) {
        *localAddr = bindAddr;
    }
    if (peerAddr != nullptr) {
        *peerAddr = cfg.peer_addr;
    }
    return true;
}

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
