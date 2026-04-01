// 6Tunnel: IPv4 over IPv6 tunnel on Windows using Wintun.
// Build target: MSYS2 + CMake (MinGW-w64).

#ifndef UNICODE
#define UNICODE
#endif

#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "hexdump.h"
#include "log.h"
#include "util.h"
#include "wintun_loader.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

constexpr DWORD kWintunRingCapacity = 0x400000;
constexpr int kMaxPacketSize = 65535;
constexpr size_t kMaxHexdumpBytes = 256;
constexpr int kSocketRecvTimeoutMs = 1000;
constexpr int kForceExitTimeoutMs = 4000;

std::atomic<bool> g_running{true};
std::atomic<bool> g_shutdownCompleted{false};
std::atomic<int> g_stopSignalCount{0};

void LogHexdumpDebug(const std::string& title, const uint8_t* data, size_t len) {
	const size_t dumpLen = (std::min)(len, kMaxHexdumpBytes);
	Log(LogLevel::Debug, title + ", hexdump bytes=" + std::to_string(dumpLen) + "/" + std::to_string(len));

	std::istringstream iss(HexdumpC(data, dumpLen));
	std::string line;
	while (std::getline(iss, line)) {
		Log(LogLevel::Debug, line);
	}

	if (dumpLen < len) {
		Log(LogLevel::Debug, "hexdump truncated (max " + std::to_string(kMaxHexdumpBytes) + " bytes)");
	}
}

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
				Log(LogLevel::Warn, "Stop signal received, shutting down... Press Ctrl+C again to force exit.");

				std::thread([]() {
					std::this_thread::sleep_for(std::chrono::milliseconds(kForceExitTimeoutMs));
					if (!g_shutdownCompleted.load()) {
						Log(LogLevel::Error, "Graceful shutdown timeout, force terminating process.");
						TerminateProcess(GetCurrentProcess(), 130);
					}
				}).detach();
			} else {
				Log(LogLevel::Error, "Second stop signal received, force terminating immediately.");
				TerminateProcess(GetCurrentProcess(), 130);
			}
			return TRUE;
		}
		default:
			return FALSE;
	}
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

	SetConsoleCtrlHandler(ConsoleHandler, TRUE);

	Log(LogLevel::Info, "6Tunnel starting...");
	Log(LogLevel::Info, "Config file: " + configPath);
	Log(LogLevel::Info, "Log level: " + std::string(LevelToString(cfg.log_level)));
	Log(LogLevel::Info, "Local IPv6: " + cfg.local_ipv6 + ", Peer IPv6: " + cfg.peer_ipv6 + ", UDP port: " + std::to_string(cfg.udp_port));
	Log(LogLevel::Info, "Adapter: " + cfg.adapter_name + ", local tun IPv4: " + cfg.local_tun_ipv4 + "/" + std::to_string(cfg.tun_prefix));

	WINTUN_ADAPTER_HANDLE adapter = nullptr;
	WINTUN_SESSION_HANDLE session = nullptr;
	SOCKET sock = INVALID_SOCKET;

	WSADATA wsa{};
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		Log(LogLevel::Error, "WSAStartup failed");
		return 1;
	}

	if (!LoadWintunLibrary(L"wintun.dll")) {
		Log(LogLevel::Error, std::string("Failed to load Wintun API: ") + GetWintunLoadError());
		WSACleanup();
		return 1;
	}

	do {
		const std::wstring wAdapterName = Utf8ToWide(cfg.adapter_name);
		const std::wstring wTunnelType = Utf8ToWide(cfg.tunnel_type);

		adapter = WtOpenAdapter(wAdapterName.c_str());
		if (adapter == nullptr) {
			Log(LogLevel::Warn, "Adapter not found, creating new adapter...");
			adapter = WtCreateAdapter(wAdapterName.c_str(), wTunnelType.c_str(), nullptr);
			if (adapter == nullptr) {
				Log(LogLevel::Error, "WintunCreateAdapter failed. last_error=" + std::to_string(GetLastError()));
				break;
			}
		} else {
			Log(LogLevel::Info, "Opened existing adapter.");
		}

		if (cfg.auto_config_ipv4) {
			if (!ConfigureTunIpv4(cfg)) {
				Log(LogLevel::Error, "Failed to set adapter IPv4. Run as administrator.");
				break;
			}
		} else {
			Log(LogLevel::Info, "auto_config_ipv4=false, skip netsh IPv4 setup.");
		}

		session = WtStartSession(adapter, kWintunRingCapacity);
		if (session == nullptr) {
			Log(LogLevel::Error, "WintunStartSession failed. last_error=" + std::to_string(GetLastError()));
			break;
		}

		sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
		if (sock == INVALID_SOCKET) {
			Log(LogLevel::Error, "socket(AF_INET6, UDP) failed. err=" + std::to_string(WSAGetLastError()));
			break;
		}

		int v6only = 1;
		setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&v6only), sizeof(v6only));
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&kSocketRecvTimeoutMs), sizeof(kSocketRecvTimeoutMs));

		sockaddr_in6 localAddr{};
		localAddr.sin6_family = AF_INET6;
		localAddr.sin6_port = htons(cfg.udp_port);
		if (!ParseIpv6(cfg.local_ipv6, &localAddr.sin6_addr)) {
			Log(LogLevel::Error, "Invalid local_ipv6: " + cfg.local_ipv6);
			break;
		}

		if (bind(sock, reinterpret_cast<const sockaddr*>(&localAddr), sizeof(localAddr)) != 0) {
			const int bindErr = WSAGetLastError();
			Log(LogLevel::Warn, "bind to configured local_ipv6 failed. err=" + std::to_string(bindErr)
				+ ", local_ipv6=" + cfg.local_ipv6 + ", fallback to ::");

			sockaddr_in6 anyAddr{};
			anyAddr.sin6_family = AF_INET6;
			anyAddr.sin6_port = htons(cfg.udp_port);
			if (!ParseIpv6("::", &anyAddr.sin6_addr)) {
				Log(LogLevel::Error, "Internal parse failure for ::");
				break;
			}

			if (bind(sock, reinterpret_cast<const sockaddr*>(&anyAddr), sizeof(anyAddr)) != 0) {
				Log(LogLevel::Error, "bind fallback to :: failed. err=" + std::to_string(WSAGetLastError()));
				break;
			}

			Log(LogLevel::Info, "Socket bound to :: successfully.");
		} else {
			Log(LogLevel::Info, "Socket bound to local_ipv6=" + cfg.local_ipv6);
		}

		sockaddr_in6 peerAddr{};
		peerAddr.sin6_family = AF_INET6;
		peerAddr.sin6_port = htons(cfg.udp_port);
		if (!ParseIpv6(cfg.peer_ipv6, &peerAddr.sin6_addr)) {
			Log(LogLevel::Error, "Invalid peer_ipv6: " + cfg.peer_ipv6);
			break;
		}

		Log(LogLevel::Info, "Data plane is up. Press Ctrl+C to stop.");

		HANDLE readEvent = WtGetReadWaitEvent(session);
		std::thread tunToNet([&]() {
			while (g_running.load()) {
				DWORD packetSize = 0;
				BYTE* packet = WtReceivePacket(session, &packetSize);
				if (!packet) {
					DWORD err = GetLastError();
					if (err == ERROR_NO_MORE_ITEMS) {
						WaitForSingleObject(readEvent, 500);
						continue;
					}
					if (err == ERROR_HANDLE_EOF) {
						Log(LogLevel::Warn, "Wintun session EOF");
						g_running.store(false);
						break;
					}
					Log(LogLevel::Error, "WintunReceivePacket failed. err=" + std::to_string(err));
					continue;
				}

				if (IsIpv4Packet(packet, packetSize)) {
					const int sent = sendto(sock,
											reinterpret_cast<const char*>(packet),
											static_cast<int>(packetSize),
											0,
											reinterpret_cast<const sockaddr*>(&peerAddr),
											sizeof(peerAddr));
					if (sent < 0) {
						Log(LogLevel::Error, "sendto failed. err=" + std::to_string(WSAGetLastError()));
					} else {
						Log(LogLevel::Debug, "TX IPv4 packet from TUN to IPv6 socket, bytes=" + std::to_string(sent));
					}
				} else {
					Log(LogLevel::Debug, "Skip non-IPv4 packet from TUN, bytes=" + std::to_string(packetSize));
					LogHexdumpDebug("TUN skip packet", packet, packetSize);
				}
				WtReleaseReceivePacket(session, packet);
			}
		});

		std::thread netToTun([&]() {
			std::vector<uint8_t> buf(kMaxPacketSize);
			while (g_running.load()) {
				sockaddr_in6 src{};
				int srcLen = sizeof(src);
				const int recvLen = recvfrom(sock,
											 reinterpret_cast<char*>(buf.data()),
											 static_cast<int>(buf.size()),
											 0,
											 reinterpret_cast<sockaddr*>(&src),
											 &srcLen);
				if (recvLen < 0) {
					const int err = WSAGetLastError();
					if (!g_running.load()) {
						break;
					}
					if (err == WSAEINTR || err == WSAEWOULDBLOCK || err == WSAETIMEDOUT) {
						continue;
					}
					Log(LogLevel::Error, "recvfrom failed. err=" + std::to_string(err));
					continue;
				}

				if (!IsIpv4Packet(buf.data(), static_cast<size_t>(recvLen))) {
					Log(LogLevel::Debug, "RX non-IPv4 frame over IPv6 socket, drop bytes=" + std::to_string(recvLen));
					LogHexdumpDebug("IPv6 socket drop packet", buf.data(), static_cast<size_t>(recvLen));
					continue;
				}

				BYTE* out = WtAllocateSendPacket(session, recvLen);
				if (!out) {
					Log(LogLevel::Warn, "WintunAllocateSendPacket failed, drop packet. err=" + std::to_string(GetLastError()));
					continue;
				}
				std::memcpy(out, buf.data(), static_cast<size_t>(recvLen));
				WtSendPacket(session, out);

				Log(LogLevel::Debug, "RX IPv4 packet from IPv6 socket to TUN, bytes=" + std::to_string(recvLen));
			}
		});

		while (g_running.load()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}

		shutdown(sock, SD_BOTH);

		if (tunToNet.joinable()) {
			tunToNet.join();
		}
		if (netToTun.joinable()) {
			netToTun.join();
		}
	} while (false);

	if (sock != INVALID_SOCKET) {
		closesocket(sock);
		sock = INVALID_SOCKET;
	}
	if (session != nullptr) {
		WtEndSession(session);
		session = nullptr;
	}
	if (adapter != nullptr) {
		WtCloseAdapter(adapter);
		adapter = nullptr;
	}

	UnloadWintunLibrary();
	WSACleanup();
	g_shutdownCompleted.store(true);
	Log(LogLevel::Info, "6Tunnel exited.");
	return 0;
}
