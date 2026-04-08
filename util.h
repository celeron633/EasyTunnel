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
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

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

// ---------------------------------------------------------------------------
// Packet / protocol helpers
// ---------------------------------------------------------------------------
bool IsIpv4Packet(const uint8_t* data, size_t len);
std::string PrefixToMask(uint8_t prefix);
bool RunCommand(const std::string& cmd);
bool ConfigureTunIpv4(const Config& cfg);
bool ConfigureTunMtu(const Config& cfg);
bool ParseIpv6(const std::string& ip, in6_addr* out);
std::string IpProtoToName(uint8_t proto);
std::string NonIpv4PacketType(const uint8_t* packet, size_t len);
std::string Ipv4ProtocolToString(const uint8_t* packet, size_t len);

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& s);
bool DisableTunIpv6(const Config& cfg);
#endif
