#include "tunnel_engine.h"

#include <chrono>
#include <cstring>
#include <sstream>
#include <vector>

#include "hexdump.h"
#include "log.h"
#include "nat_traversal.h"
#include "rendezvous_client.h"

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

const char* TunnelStateName(TunnelState state) {
	switch (state) {
		case TunnelState::Disconnected: return "Disconnected";
		case TunnelState::Connecting: return "Connecting";
		case TunnelState::Connected: return "Connected";
		case TunnelState::Error: return "Error";
		default: return "Unknown";
	}
}

std::string CurrentThreadId() {
	std::ostringstream output;
	output << std::this_thread::get_id();
	return output.str();
}
}  // namespace

TunnelEngine::TunnelEngine() = default;

TunnelEngine::~TunnelEngine() {
	Stop();
}

bool TunnelEngine::Start(const Config& cfg) {
	if (running_.load()) {
		Log(LogLevel::Warn, "TunnelEngine::Start ignored: engine is already running");
		return false;
	}
	if (workerThread_.joinable()) {
		Log(LogLevel::Debug, "Joining previous tunnel worker before restart");
		workerThread_.join();
	}

	// Reset stats
	stats_.txPackets.store(0);
	stats_.rxPackets.store(0);
	stats_.txBytes.store(0);
	stats_.rxBytes.store(0);
	stats_.rttMilliseconds.store(-1);

	running_.store(true);
	SetState(TunnelState::Connecting);

	workerThread_ = std::thread(&TunnelEngine::WorkerThread, this, cfg);
	return true;
}

void TunnelEngine::Stop() {
	const bool wasRunning = running_.exchange(false);
	Log(LogLevel::Debug, "TunnelEngine::Stop requested: was_running="
		+ std::string(wasRunning ? "true" : "false")
		+ ", state=" + TunnelStateName(state_.load())
		+ ", caller_thread=" + CurrentThreadId());
	if (workerThread_.joinable()) {
		workerThread_.join();
		Log(LogLevel::Debug, "TunnelEngine::Stop joined worker thread");
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
	const TunnelState previous = state_.exchange(state);
	Log(LogLevel::Debug, "Tunnel state " + std::string(TunnelStateName(previous))
		+ " -> " + TunnelStateName(state)
		+ (msg.empty() ? "" : ": " + msg));
	std::lock_guard<std::mutex> lock(cbMutex_);
	if (stateCallback_) {
		stateCallback_(state, msg);
	}
}

void TunnelEngine::WorkerThread(Config cfg) {
	SetLogLevel(cfg.log_level);

	Log(LogLevel::Info, "EasyTunnel engine starting... worker_thread="
		+ CurrentThreadId());
	Log(LogLevel::Info,
		"Rendezvous: " + cfg.rendezvous_addr + ":"
		+ std::to_string(cfg.rendezvous_port));
	Log(LogLevel::Debug, "Engine config: mode="
		+ std::string(cfg.target_peer_id.empty() ? "wait" : "connect")
		+ ", peer_id=" + cfg.peer_id
		+ (cfg.target_peer_id.empty() ? "" : ", target_peer_id=" + cfg.target_peer_id)
		+ ", punch_timeout=" + std::to_string(cfg.punch_timeout) + "s"
		+ ", nat4_source_ports=" + std::to_string(cfg.nat4_source_port_start)
		+ "+" + std::to_string(cfg.nat4_source_port_count)
		+ ", nat4_peer_port_offset=" + std::to_string(cfg.nat4_peer_port_offset));

	socket_t sock = kInvalidSocket;
	UdpEndpoint server{};
	std::unique_ptr<TunAdapter> tunAdapter(TunAdapter::Create());
	std::string exitReason = "worker completed";
	std::mutex exitReasonMutex;
	auto setExitReason = [&](const std::string& reason) {
		std::lock_guard<std::mutex> lock(exitReasonMutex);
		exitReason = reason;
	};
	auto getExitReason = [&]() {
		std::lock_guard<std::mutex> lock(exitReasonMutex);
		return exitReason;
	};

	do {
		UdpEndpoint peer{};
		std::string socketError;
		if (!OpenRendezvousSocket(cfg, kSocketRecvTimeoutMs, &sock, &server, &socketError)) {
			setExitReason("failed to open rendezvous socket");
			Log(LogLevel::Error, getExitReason() + ": " + socketError);
			SetState(TunnelState::Error, socketError);
			break;
		}
		Log(LogLevel::Debug, "Opened rendezvous UDP socket; resolved_server="
			+ FormatUdpEndpoint(server) + ", recv_timeout_ms="
			+ std::to_string(kSocketRecvTimeoutMs));
		SetState(TunnelState::Connecting,
			cfg.target_peer_id.empty() ? "Registered; waiting for a peer"
			                           : "Connecting to " + cfg.target_peer_id);
		if (!DiscoverAndPunch(&sock, cfg, server, running_, &peer, &socketError)) {
			if (!running_.load()) {
				setExitReason("stop requested during NAT traversal");
				Log(LogLevel::Debug, getExitReason() + ": " + socketError);
			} else {
				setExitReason("NAT traversal failed");
				Log(LogLevel::Error, getExitReason() + ": " + socketError);
				SetState(TunnelState::Error, socketError);
			}
			break;
		}

		if (!tunAdapter->Open(cfg)) {
			setExitReason("failed to open TUN adapter");
			Log(LogLevel::Error, getExitReason());
			SetState(TunnelState::Error, "Failed to open TUN adapter");
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
					setExitReason("TUN read failed");
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
			auto lastPeerSeen = std::chrono::steady_clock::now();
			auto nextKeepalive = lastPeerSeen + std::chrono::seconds(cfg.keepalive_interval);
			auto nextDummyTraffic = lastPeerSeen;
			std::chrono::steady_clock::time_point keepaliveSentAt{};
			std::string pendingKeepaliveId;
			uint64_t keepaliveSequence = 0;
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
					if (!IsRecvTimeout(err)) {
						Log(IsUdpDestinationUnreachable(err) ? LogLevel::Debug : LogLevel::Error,
							"Data-plane recvfrom failed. err=" + std::to_string(err)
							+ (IsUdpDestinationUnreachable(err)
								? "; transient UDP unreachable ignored" : ""));
					}
				} else {
					UdpEndpoint source{};
					source.addr = src;
					source.addr_len = srcLen;
					source.family = src.ss_family;
					const PeerControlResult control = HandlePeerControl(
						sock, cfg, peer, source, buf.data(), static_cast<size_t>(recvLen));
					if (control.handled) {
						// control packet consumed
						const auto receivedAt = std::chrono::steady_clock::now();
						if (control.peerSeen) lastPeerSeen = receivedAt;
						if (control.receivedKeepaliveAck && !pendingKeepaliveId.empty()
							&& (control.keepaliveAckId.empty()
								|| control.keepaliveAckId == pendingKeepaliveId)) {
							const auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
								receivedAt - keepaliveSentAt).count();
							stats_.rttMilliseconds.store(rtt);
							pendingKeepaliveId.clear();
						}
						if (control.consumedDummyTraffic) {
							stats_.rxPackets.fetch_add(1);
							stats_.rxBytes.fetch_add(static_cast<uint64_t>(recvLen));
						}
					} else if (!SameUdpEndpoint(source, peer)) {
						Log(LogLevel::Warn, "Drop UDP packet from unexpected source "
							+ FormatUdpEndpoint(source));
					} else if (!IsIpv4Packet(buf.data(), static_cast<size_t>(recvLen))) {
						Log(LogLevel::Debug, "RX non-IPv4 drop, bytes=" + std::to_string(recvLen));
					} else if (tunAdapter->WritePacket(buf.data(), static_cast<size_t>(recvLen))) {
						lastPeerSeen = std::chrono::steady_clock::now();
						stats_.rxPackets.fetch_add(1);
						stats_.rxBytes.fetch_add(static_cast<uint64_t>(recvLen));
						Log(LogLevel::Debug, "RX IPv4 ["
							+ Ipv4ProtocolToString(buf.data(), static_cast<size_t>(recvLen))
							+ "] bytes=" + std::to_string(recvLen));
					}
				}

				const auto now = std::chrono::steady_clock::now();
				if (now >= nextKeepalive) {
					const std::string requestId = std::to_string(++keepaliveSequence);
					if (SendPeerKeepalive(sock, cfg, peer, requestId)) {
						keepaliveSentAt = std::chrono::steady_clock::now();
						pendingKeepaliveId = requestId;
					}
					nextKeepalive = now + std::chrono::seconds(cfg.keepalive_interval);
				}
				if (cfg.dummy_traffic_enabled && now >= nextDummyTraffic) {
					if (SendPeerDummyTraffic(sock, cfg, peer)) {
						stats_.txPackets.fetch_add(1);
						stats_.txBytes.fetch_add(kPeerDummyTrafficPacketSize);
					}
					nextDummyTraffic = now + std::chrono::seconds(1);
				}
				if (now - lastPeerSeen > std::chrono::seconds(cfg.peer_timeout)) {
					const std::string timeoutError =
						"Peer keepalive timed out; NAT mapping may be lost";
					Log(LogLevel::Error, timeoutError);
					setExitReason("peer keepalive timed out");
					SetState(TunnelState::Error, timeoutError);
					running_.store(false);
					break;
				}
				if (recvLen < 0) {
					continue;
				}
			}
		});

		while (running_.load()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}
		if (getExitReason() == "worker completed") setExitReason("stop requested");
		Log(LogLevel::Debug, "Data-plane shutdown started: reason=" + getExitReason());

		ShutdownSocket(sock);

		if (tunToNet.joinable()) tunToNet.join();
		if (netToTun.joinable()) netToTun.join();

	} while (false);

	running_.store(false);
	if (sock != kInvalidSocket) UnregisterRendezvous(sock, cfg, server);
	CloseSocket(sock);
	if (tunAdapter) {
		tunAdapter->Close();
	}

	if (state_.load() != TunnelState::Error) {
		SetState(TunnelState::Disconnected, "Tunnel stopped");
	}

	Log(LogLevel::Info, "EasyTunnel engine stopped. reason=" + getExitReason()
		+ ", final_state=" + TunnelStateName(state_.load()));
}
