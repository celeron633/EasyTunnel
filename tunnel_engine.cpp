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

	Log(LogLevel::Info, "EasyTunnel engine starting...");
	Log(LogLevel::Info,
		"Local address: " + (cfg.local_addr.empty() ? "auto" : cfg.local_addr)
		+ ", peer address: " + cfg.peer_addr
		+ ", UDP port: " + std::to_string(cfg.udp_port));

	socket_t sock = kInvalidSocket;
	std::unique_ptr<TunAdapter> tunAdapter(TunAdapter::Create());

	do {
		if (!tunAdapter->Open(cfg)) {
			SetState(TunnelState::Error, "Failed to open TUN adapter");
			break;
		}

		UdpEndpoint peer{};
		std::string boundAddr;
		std::string peerAddr;
		std::string socketError;
		if (!OpenUdpSocket(cfg, kSocketRecvTimeoutMs, &sock, &peer,
				&boundAddr, &peerAddr, &socketError)) {
			SetState(TunnelState::Error, socketError);
			break;
		}
		Log(LogLevel::Info,
			"UDP socket bound to " + AddressFamilyName(peer.family)
			+ " local=" + boundAddr + ", peer=" + peerAddr);

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
						reinterpret_cast<const sockaddr*>(&peer.addr),
						peer.addr_len);
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

	Log(LogLevel::Info, "EasyTunnel engine stopped.");
}
