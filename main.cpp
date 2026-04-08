// 6Tunnel: IPv4 over IPv6 tunnel.
// Supports Windows (Wintun) and Linux (/dev/net/tun).

#ifdef _WIN32

#ifndef UNICODE
#define UNICODE
#endif

#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

#else  // Linux / POSIX

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>

#endif  // _WIN32

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "hexdump.h"
#include "log.h"
#include "tun_adapter.h"
#include "util.h"

// ---------------------------------------------------------------------------
// Platform type aliases for sockets
// ---------------------------------------------------------------------------
#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

namespace {

constexpr int kMaxPacketSize = 65535;
constexpr int kSocketRecvTimeoutMs = 1000;
#ifdef _WIN32
constexpr int kForceExitTimeoutMs = 4000;
#endif

std::atomic<bool> g_running{true};
std::atomic<bool> g_shutdownCompleted{false};
std::atomic<int> g_stopSignalCount{0};

// ---------------------------------------------------------------------------
// Platform-specific signal / console-event handling
// ---------------------------------------------------------------------------
#ifdef _WIN32

BOOL WINAPI ConsoleHandler(DWORD signal) {
	switch (signal) {
		case CTRL_C_EVENT:
		case CTRL_BREAK_EVENT:
		case CTRL_CLOSE_EVENT:
		case CTRL_LOGOFF_EVENT:
		case CTRL_SHUTDOWN_EVENT:
		{
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

#else  // POSIX

void SignalHandler(int /*sig*/) {
	const int count = g_stopSignalCount.fetch_add(1) + 1;
	if (count == 1) {
		g_running.store(false);
	} else {
		std::_Exit(130);
	}
}

#endif  // _WIN32

// ---------------------------------------------------------------------------
// Helpers that paper over Winsock vs POSIX differences
// ---------------------------------------------------------------------------

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
	return err == EAGAIN || err == EWOULDBLOCK || err == EINTR
		|| err == ETIMEDOUT;
#endif
}

}  // namespace

int main(int argc, char** argv) {
	const std::string configPath = (argc >= 2) ? argv[1] : "tunnel.conf";
	Config cfg;
	if (!LoadConfig(configPath, &cfg)) {
		Log(LogLevel::Error, "Load config failed.");
		return 1;
	}

	SetLogLevel(cfg.log_level);

#ifdef _WIN32
	SetConsoleCtrlHandler(ConsoleHandler, TRUE);
#else
	struct sigaction sa {};
	sa.sa_handler = SignalHandler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, nullptr);
	sigaction(SIGTERM, &sa, nullptr);
#endif

	Log(LogLevel::Info, "6Tunnel starting...");
	Log(LogLevel::Info, "Config file: " + configPath);
	Log(LogLevel::Info,
		"Log level: " + std::string(LevelToString(cfg.log_level)));
	Log(LogLevel::Info,
		"Local IPv6: " + cfg.local_ipv6 + ", Peer IPv6: " + cfg.peer_ipv6
		+ ", UDP port: " + std::to_string(cfg.udp_port));
	Log(LogLevel::Info,
		"Adapter: " + cfg.adapter_name
		+ ", local tun IPv4: " + cfg.local_tun_ipv4
		+ "/" + std::to_string(cfg.tun_prefix)
		+ ", tun MTU: " + std::to_string(cfg.tun_mtu));

	socket_t sock = kInvalidSocket;
	std::unique_ptr<TunAdapter> tunAdapter(TunAdapter::Create());

#ifdef _WIN32
	WSADATA wsa{};
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		Log(LogLevel::Error, "WSAStartup failed");
		return 1;
	}
#endif

	do {
		if (!tunAdapter->Open(cfg)) {
			break;
		}

		sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
		if (sock == kInvalidSocket) {
			Log(LogLevel::Error,
				"socket(AF_INET6, UDP) failed. err="
				+ std::to_string(GetSocketError()));
			break;
		}

		int v6only = 1;
		setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY,
			reinterpret_cast<char*>(&v6only), sizeof(v6only));

#ifdef _WIN32
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
			reinterpret_cast<const char*>(&kSocketRecvTimeoutMs),
			sizeof(kSocketRecvTimeoutMs));
#else
		struct timeval tv;
		tv.tv_sec = kSocketRecvTimeoutMs / 1000;
		tv.tv_usec = (kSocketRecvTimeoutMs % 1000) * 1000;
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

		sockaddr_in6 localAddr{};
		localAddr.sin6_family = AF_INET6;
		localAddr.sin6_port = htons(cfg.udp_port);
		if (!ParseIpv6(cfg.local_ipv6, &localAddr.sin6_addr)) {
			Log(LogLevel::Error, "Invalid local_ipv6: " + cfg.local_ipv6);
			break;
		}

		if (bind(sock, reinterpret_cast<const sockaddr*>(&localAddr),
				sizeof(localAddr)) != 0) {
			const int bindErr = GetSocketError();
			Log(LogLevel::Warn,
				"bind to configured local_ipv6 failed. err="
				+ std::to_string(bindErr)
				+ ", local_ipv6=" + cfg.local_ipv6 + ", fallback to ::");

			sockaddr_in6 anyAddr{};
			anyAddr.sin6_family = AF_INET6;
			anyAddr.sin6_port = htons(cfg.udp_port);
			if (!ParseIpv6("::", &anyAddr.sin6_addr)) {
				Log(LogLevel::Error, "Internal parse failure for ::");
				break;
			}

			if (bind(sock, reinterpret_cast<const sockaddr*>(&anyAddr),
					sizeof(anyAddr)) != 0) {
				Log(LogLevel::Error,
					"bind fallback to :: failed. err="
					+ std::to_string(GetSocketError()));
				break;
			}

			Log(LogLevel::Info, "Socket bound to :: successfully.");
		} else {
			Log(LogLevel::Info,
				"Socket bound to local_ipv6=" + cfg.local_ipv6);
		}

		sockaddr_in6 peerAddr{};
		peerAddr.sin6_family = AF_INET6;
		peerAddr.sin6_port = htons(cfg.udp_port);
		if (!ParseIpv6(cfg.peer_ipv6, &peerAddr.sin6_addr)) {
			Log(LogLevel::Error, "Invalid peer_ipv6: " + cfg.peer_ipv6);
			break;
		}

		Log(LogLevel::Info, "Data plane is up. Press Ctrl+C to stop.");

		// TUN → network thread
		std::thread tunToNet([&]() {
			std::vector<uint8_t> buf(kMaxPacketSize);
			while (g_running.load()) {
				size_t pktLen = 0;
				if (!tunAdapter->ReadPacket(buf.data(), buf.size(), pktLen)) {
					if (!g_running.load()) {
						break;
					}
					continue;
				}
				if (pktLen == 0) {
					continue;  // timeout
				}

				if (IsIpv4Packet(buf.data(), pktLen)) {
					const int sent = sendto(
						sock,
						reinterpret_cast<const char*>(buf.data()),
						static_cast<int>(pktLen),
						0,
						reinterpret_cast<const sockaddr*>(&peerAddr),
						sizeof(peerAddr));
					if (sent < 0) {
						Log(LogLevel::Error,
							"sendto failed. err="
							+ std::to_string(GetSocketError()));
					} else {
						Log(LogLevel::Debug,
							"TX IPv4 ["
							+ Ipv4ProtocolToString(buf.data(), pktLen)
							+ "] packet from TUN to IPv6 socket, bytes="
							+ std::to_string(sent));
					}
				} else {
					Log(LogLevel::Debug,
						"Skip non-IPv4 ["
						+ NonIpv4PacketType(buf.data(), pktLen)
						+ "] packet from TUN, bytes="
						+ std::to_string(pktLen));
					LogHexdumpDebug("TUN skip packet", buf.data(), pktLen);
				}
			}
		});

		// network → TUN thread
		std::thread netToTun([&]() {
			std::vector<uint8_t> buf(kMaxPacketSize);
			while (g_running.load()) {
				sockaddr_in6 src{};
#ifdef _WIN32
				int srcLen = sizeof(src);
#else
				socklen_t srcLen = sizeof(src);
#endif
				const int recvLen = recvfrom(
					sock,
					reinterpret_cast<char*>(buf.data()),
					static_cast<int>(buf.size()),
					0,
					reinterpret_cast<sockaddr*>(&src),
					&srcLen);
				if (recvLen < 0) {
					const int err = GetSocketError();
					if (!g_running.load()) {
						break;
					}
					if (IsRecvTimeout(err)) {
						continue;
					}
					Log(LogLevel::Error,
						"recvfrom failed. err=" + std::to_string(err));
					continue;
				}

				if (!IsIpv4Packet(buf.data(),
						static_cast<size_t>(recvLen))) {
					Log(LogLevel::Debug,
						"RX non-IPv4 ["
						+ NonIpv4PacketType(buf.data(),
							static_cast<size_t>(recvLen))
						+ "] frame over IPv6 socket, drop bytes="
						+ std::to_string(recvLen));
					LogHexdumpDebug("IPv6 socket drop packet", buf.data(),
						static_cast<size_t>(recvLen));
					continue;
				}

				if (!tunAdapter->WritePacket(buf.data(),
						static_cast<size_t>(recvLen))) {
					continue;
				}

				Log(LogLevel::Debug,
					"RX IPv4 ["
					+ Ipv4ProtocolToString(buf.data(),
						static_cast<size_t>(recvLen))
					+ "] packet from IPv6 socket to TUN, bytes="
					+ std::to_string(recvLen));
			}
		});

		while (g_running.load()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}

#ifdef _WIN32
		shutdown(sock, SD_BOTH);
#else
		shutdown(sock, SHUT_RDWR);
#endif

		if (tunToNet.joinable()) {
			tunToNet.join();
		}
		if (netToTun.joinable()) {
			netToTun.join();
		}
	} while (false);

	CloseSocket(sock);
	if (tunAdapter) {
		tunAdapter->Close();
	}

#ifdef _WIN32
	WSACleanup();
#endif

	g_shutdownCompleted.store(true);
	Log(LogLevel::Info, "6Tunnel exited.");
	return 0;
}
