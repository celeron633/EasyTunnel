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
#include <netdb.h>
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

bool ResolveUdpEndpoint(const std::string& host, uint16_t port, int family,
                        UdpEndpoint* out, std::string* error) {
    if (ParseUdpEndpoint(host, port, out)
        && (family == AF_UNSPEC || out->family == family)) {
        return true;
    }
    addrinfo hints{};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    const int rc = getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
    if (rc != 0 || result == nullptr) {
        if (error) *error = "Cannot resolve " + host + ":" + service;
        if (result) freeaddrinfo(result);
        return false;
    }
    UdpEndpoint endpoint{};
    endpoint.family = result->ai_family;
    endpoint.addr_len = static_cast<socket_len_t>(result->ai_addrlen);
    std::memcpy(&endpoint.addr, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);
    *out = endpoint;
    return true;
}

std::string FormatUdpEndpoint(const UdpEndpoint& endpoint) {
    char host[INET6_ADDRSTRLEN]{};
    uint16_t port = 0;
    if (endpoint.family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(&endpoint.addr);
        inet_ntop(AF_INET, &a->sin_addr, host, sizeof(host));
        port = ntohs(a->sin_port);
        return std::string(host) + ":" + std::to_string(port);
    }
    const auto* a = reinterpret_cast<const sockaddr_in6*>(&endpoint.addr);
    inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof(host));
    port = ntohs(a->sin6_port);
    return "[" + std::string(host) + "]:" + std::to_string(port);
}

bool SameUdpEndpoint(const UdpEndpoint& a, const UdpEndpoint& b) {
    if (a.family != b.family) return false;
    if (a.family == AF_INET) {
        const auto* x = reinterpret_cast<const sockaddr_in*>(&a.addr);
        const auto* y = reinterpret_cast<const sockaddr_in*>(&b.addr);
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    const auto* x = reinterpret_cast<const sockaddr_in6*>(&a.addr);
    const auto* y = reinterpret_cast<const sockaddr_in6*>(&b.addr);
    return x->sin6_port == y->sin6_port
        && std::memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(in6_addr)) == 0;
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
