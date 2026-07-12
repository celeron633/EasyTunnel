// EasyTunnel: IPv4 tunnel over UDP.
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

#endif  // _WIN32

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

namespace {
constexpr int kMaxPacketSize = 65535;
constexpr int kSocketRecvTimeoutMs = 1000;
}  // namespace

int main(int argc, char** argv) {
	const std::string configPath = (argc >= 2) ? argv[1] : "tunnel.conf";
	Config cfg;
	if (!LoadConfig(configPath, &cfg)) {
		Log(LogLevel::Error, "Load config failed.");
		return 1;
	}

	SetLogLevel(cfg.log_level);
	RegisterSignalHandlers();

	Log(LogLevel::Info, "EasyTunnel starting...");
	Log(LogLevel::Info, "Config file: " + configPath);
	Log(LogLevel::Info,
		"Log level: " + std::string(LevelToString(cfg.log_level)));
	Log(LogLevel::Info,
		"Local address: " + (cfg.local_addr.empty() ? "auto" : cfg.local_addr)
		+ ", peer address: " + cfg.peer_addr
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

		UdpEndpoint peer{};
		std::string boundAddr;
		std::string peerAddr;
		std::string socketError;
		if (!OpenUdpSocket(cfg, kSocketRecvTimeoutMs, &sock, &peer,
				&boundAddr, &peerAddr, &socketError)) {
			Log(LogLevel::Error, socketError);
			break;
		}
		Log(LogLevel::Info,
			"UDP socket bound to " + AddressFamilyName(peer.family)
			+ " local=" + boundAddr + ", peer=" + peerAddr);

		Log(LogLevel::Info, "Data plane is up. Press Ctrl+C to stop.");

		// TUN -> network thread
		std::thread tunToNet([&]() {
			std::vector<uint8_t> buf(kMaxPacketSize);
			while (g_running.load()) {
				size_t pktLen = 0;
				if (!tunAdapter->ReadPacket(buf.data(), buf.size(), pktLen)) {
					Log(LogLevel::Error,
						"TUN read failed; stopping data plane.");
					g_running.store(false);
					break;
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
						reinterpret_cast<const sockaddr*>(&peer.addr),
						peer.addr_len);
					if (sent < 0) {
						Log(LogLevel::Error,
							"sendto failed. err="
							+ std::to_string(GetSocketError()));
					} else {
						Log(LogLevel::Debug,
							"TX IPv4 ["
							+ Ipv4ProtocolToString(buf.data(), pktLen)
							+ "] packet from TUN to UDP socket, bytes="
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

		// network -> TUN thread
		std::thread netToTun([&]() {
			std::vector<uint8_t> buf(kMaxPacketSize);
			while (g_running.load()) {
				sockaddr_storage src{};
				socket_len_t srcLen = static_cast<socket_len_t>(sizeof(src));
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
						+ "] frame over UDP socket, drop bytes="
						+ std::to_string(recvLen));
					LogHexdumpDebug("UDP socket drop packet", buf.data(),
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
					+ "] packet from UDP socket to TUN, bytes="
					+ std::to_string(recvLen));
			}
		});

		while (g_running.load()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}

		ShutdownSocket(sock);

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
	Log(LogLevel::Info, "EasyTunnel exited.");
	return 0;
}
