#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "config.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

// ---------------------------------------------------------------------------
// Cross-platform socket type aliases
// ---------------------------------------------------------------------------
#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
using socket_len_t = int;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
using socket_len_t = socklen_t;
#endif

struct UdpEndpoint {
    sockaddr_storage addr{};
    socket_len_t addr_len = 0;
    int family = AF_UNSPEC;
};

// ---------------------------------------------------------------------------
// Program control state (defined in util.cpp, shared with signal handlers)
// ---------------------------------------------------------------------------
extern std::atomic<bool> g_running;
extern std::atomic<bool> g_shutdownCompleted;

// ---------------------------------------------------------------------------
// Signal / console-event handler registration.
// Hooks SIGINT/SIGTERM (POSIX) or SetConsoleCtrlHandler (Windows).
// Must be called once at startup.
// ---------------------------------------------------------------------------
void RegisterSignalHandlers();

// ---------------------------------------------------------------------------
// Cross-platform socket helpers
// ---------------------------------------------------------------------------
void SetSocketRecvTimeoutMs(socket_t sock, int timeoutMs);
void ShutdownSocket(socket_t sock);
void CloseSocket(socket_t& sock);
int GetSocketError();
bool IsRecvTimeout(int err);
bool IsUdpDestinationUnreachable(int err);

// ---------------------------------------------------------------------------
// Packet / protocol helpers
// ---------------------------------------------------------------------------
bool IsIpv4Packet(const uint8_t* data, size_t len);
std::string PrefixToMask(uint8_t prefix);
bool RunCommand(const std::string& cmd);
bool ConfigureTunIpv4(const Config& cfg);
bool ConfigureTunMtu(const Config& cfg);
bool ParseIpv4(const std::string& ip, in_addr* out);
bool ParseIpv6(const std::string& ip, in6_addr* out);
bool ParseUdpEndpoint(const std::string& ip, uint16_t port, UdpEndpoint* out);
bool ResolveUdpEndpoint(const std::string& host, uint16_t port, int family,
                        UdpEndpoint* out, std::string* error);
std::string FormatUdpEndpoint(const UdpEndpoint& endpoint);
bool SameUdpEndpoint(const UdpEndpoint& a, const UdpEndpoint& b);
bool ValidateIpAddress(const std::string& ip);
std::string AddressFamilyName(int family);
std::string IpProtoToName(uint8_t proto);
std::string NonIpv4PacketType(const uint8_t* packet, size_t len);
std::string Ipv4ProtocolToString(const uint8_t* packet, size_t len);

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& s);
bool DisableTunIpv6(const Config& cfg);
#endif
