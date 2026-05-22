#include "tunnel_engine.h"

#include <chrono>
#include <cstring>
#include <vector>

#include "hexdump.h"
#include "log.h"

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
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace {
constexpr int kMaxPacketSize = 65535;
constexpr int kSocketRecvTimeoutMs = 1000;
}  // namespace

TunnelEngine::TunnelEngine() = default;

TunnelEngine::~TunnelEngine() {
	Stop();
}

bool TunnelEngine::Start(const Config& cfg) {
	if (running_.load()) {
		return false;
	}

	// Reset stats
	stats_.txPackets.store(0);
	stats_.rxPackets.store(0);
	stats_.txBytes.store(0);
	stats_.rxBytes.store(0);

	running_.store(true);
	SetState(TunnelState::Connecting);

	workerThread_ = std::thread(&TunnelEngine::WorkerThread, this, cfg);
	return true;
}

void TunnelEngine::Stop() {
	running_.store(false);
	if (workerThread_.joinable()) {
		workerThread_.join();
	}
}

TunnelState TunnelEngine::GetState() const {
	return state_.load();
}

void TunnelEngine::SetStateCallback(StateCallback cb) {
	std::lock_guard<std::mutex> lock(cbMutex_);
	stateCallback_ = std::move(cb);
}

void TunnelEngine::SetState(TunnelState state, const std::string& msg) {
	state_.store(state);
	std::lock_guard<std::mutex> lock(cbMutex_);
	if (stateCallback_) {
		stateCallback_(state, msg);
	}
}

void TunnelEngine::WorkerThread(Config cfg) {
	SetLogLevel(cfg.log_level);

	Log(LogLevel::Info, "6Tunnel engine starting...");
	Log(LogLevel::Info,
		"Local IPv6: " + cfg.local_ipv6 + ", Peer IPv6: " + cfg.peer_ipv6
		+ ", UDP port: " + std::to_string(cfg.udp_port));

	socket_t sock = kInvalidSocket;
	std::unique_ptr<TunAdapter> tunAdapter(TunAdapter::Create());

	do {
		if (!tunAdapter->Open(cfg)) {
			SetState(TunnelState::Error, "Failed to open TUN adapter");
			break;
		}

		sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
		if (sock == kInvalidSocket) {
			SetState(TunnelState::Error,
				"socket(AF_INET6, UDP) failed. err=" + std::to_string(GetSocketError()));
			break;
		}

		int v6only = 1;
		setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY,
			reinterpret_cast<char*>(&v6only), sizeof(v6only));
		SetSocketRecvTimeoutMs(sock, kSocketRecvTimeoutMs);

		sockaddr_in6 localAddr{};
		localAddr.sin6_family = AF_INET6;
		localAddr.sin6_port = htons(cfg.udp_port);
		if (!ParseIpv6(cfg.local_ipv6, &localAddr.sin6_addr)) {
			SetState(TunnelState::Error, "Invalid local_ipv6: " + cfg.local_ipv6);
			break;
		}

		if (bind(sock, reinterpret_cast<const sockaddr*>(&localAddr),
				sizeof(localAddr)) != 0) {
			Log(LogLevel::Warn,
				"bind to configured local_ipv6 failed, fallback to ::");

			sockaddr_in6 anyAddr{};
			anyAddr.sin6_family = AF_INET6;
			anyAddr.sin6_port = htons(cfg.udp_port);
			if (!ParseIpv6("::", &anyAddr.sin6_addr)) {
				SetState(TunnelState::Error, "Internal parse failure for ::");
				break;
			}

			if (bind(sock, reinterpret_cast<const sockaddr*>(&anyAddr),
					sizeof(anyAddr)) != 0) {
				SetState(TunnelState::Error,
					"bind fallback to :: failed. err=" + std::to_string(GetSocketError()));
				break;
			}
		}

		sockaddr_in6 peerAddr{};
		peerAddr.sin6_family = AF_INET6;
		peerAddr.sin6_port = htons(cfg.udp_port);
		if (!ParseIpv6(cfg.peer_ipv6, &peerAddr.sin6_addr)) {
			SetState(TunnelState::Error, "Invalid peer_ipv6: " + cfg.peer_ipv6);
			break;
		}

		SetState(TunnelState::Connected, "Data plane is up");
		Log(LogLevel::Info, "Data plane is up.");

		// TUN -> network thread
		std::thread tunToNet([&]() {
			std::vector<uint8_t> buf(kMaxPacketSize);
			while (running_.load()) {
				size_t pktLen = 0;
				if (!tunAdapter->ReadPacket(buf.data(), buf.size(), pktLen)) {
					Log(LogLevel::Error, "TUN read failed; stopping.");
					running_.store(false);
					break;
				}
				if (pktLen == 0) {
					continue;
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
							"sendto failed. err=" + std::to_string(GetSocketError()));
					} else {
						stats_.txPackets.fetch_add(1);
						stats_.txBytes.fetch_add(static_cast<uint64_t>(sent));
						Log(LogLevel::Debug,
							"TX IPv4 [" + Ipv4ProtocolToString(buf.data(), pktLen)
							+ "] bytes=" + std::to_string(sent));
					}
				} else {
					Log(LogLevel::Debug,
						"Skip non-IPv4 [" + NonIpv4PacketType(buf.data(), pktLen)
						+ "] from TUN, bytes=" + std::to_string(pktLen));
				}
			}
		});

		// Network -> TUN thread
		std::thread netToTun([&]() {
			std::vector<uint8_t> buf(kMaxPacketSize);
			while (running_.load()) {
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
					if (!running_.load()) break;
					if (IsRecvTimeout(err)) continue;
					Log(LogLevel::Error, "recvfrom failed. err=" + std::to_string(err));
					continue;
				}

				if (!IsIpv4Packet(buf.data(), static_cast<size_t>(recvLen))) {
					Log(LogLevel::Debug,
						"RX non-IPv4 drop, bytes=" + std::to_string(recvLen));
					continue;
				}

				if (!tunAdapter->WritePacket(buf.data(), static_cast<size_t>(recvLen))) {
					continue;
				}

				stats_.rxPackets.fetch_add(1);
				stats_.rxBytes.fetch_add(static_cast<uint64_t>(recvLen));
				Log(LogLevel::Debug,
					"RX IPv4 [" + Ipv4ProtocolToString(buf.data(), static_cast<size_t>(recvLen))
					+ "] bytes=" + std::to_string(recvLen));
			}
		});

		while (running_.load()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}

		ShutdownSocket(sock);

		if (tunToNet.joinable()) tunToNet.join();
		if (netToTun.joinable()) netToTun.join();

	} while (false);

	CloseSocket(sock);
	if (tunAdapter) {
		tunAdapter->Close();
	}

	if (state_.load() != TunnelState::Error) {
		SetState(TunnelState::Disconnected, "Tunnel stopped");
	}

	Log(LogLevel::Info, "6Tunnel engine stopped.");
}
