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
	Connected,
	Error
};

struct TunnelStats {
	std::atomic<uint64_t> txPackets{0};
	std::atomic<uint64_t> rxPackets{0};
	std::atomic<uint64_t> txBytes{0};
	std::atomic<uint64_t> rxBytes{0};
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
