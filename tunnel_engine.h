#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "tun_adapter.h"
#include "util.h"

// ---------------------------------------------------------------------------
// TunnelEngine: encapsulates the tunnel data-plane logic.
// Decoupled from any UI – controlled via Start/Stop and observed via Stats
// and state callbacks.
// ---------------------------------------------------------------------------

enum class TunnelState {
	Disconnected,
	Connecting,
	// Registered with the rendezvous server without a target peer: nothing is
	// being negotiated until somebody else shows up. Kept apart from
	// Connecting so the UI does not claim a handshake is in flight.
	Waiting,
	Connected,
	Error
};

// True while the engine is running, i.e. everything but Disconnected/Error.
inline bool IsTunnelActive(TunnelState state) {
	return state == TunnelState::Connecting || state == TunnelState::Waiting
		|| state == TunnelState::Connected;
}

struct TunnelStats {
	std::atomic<uint64_t> txPackets{0};
	std::atomic<uint64_t> rxPackets{0};
	std::atomic<uint64_t> txBytes{0};
	std::atomic<uint64_t> rxBytes{0};
	// Packets received from the peer but dropped because Wintun's send ring
	// had no free space. This is non-fatal and indicates local backpressure.
	std::atomic<uint64_t> tunRingFullDrops{0};
	// Latest heartbeat round-trip time. -1 means no sample is available yet.
	std::atomic<int64_t> rttMilliseconds{-1};
};

class TunnelEngine {
public:
	using StateCallback = std::function<void(TunnelState, const std::string& msg)>;

	TunnelEngine();
	~TunnelEngine();

	// Non-copyable
	TunnelEngine(const TunnelEngine&) = delete;
	TunnelEngine& operator=(const TunnelEngine&) = delete;

	// Start the tunnel with the given config. Non-blocking.
	bool Start(const Config& cfg);

	// Stop the tunnel gracefully. Blocks until threads finish.
	void Stop();

	// Current state
	TunnelState GetState() const;

	// Packet statistics (thread-safe reads via atomics)
	const TunnelStats& GetStats() const { return stats_; }

	// Set callback for state changes (called from worker thread)
	void SetStateCallback(StateCallback cb);

private:
	void SetState(TunnelState state, const std::string& msg = "");
	void WorkerThread(Config cfg);

	std::atomic<TunnelState> state_{TunnelState::Disconnected};
	TunnelStats stats_;
	StateCallback stateCallback_;
	std::mutex cbMutex_;

	std::atomic<bool> running_{false};
	std::thread workerThread_;
};
