#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "registry.h"

struct RendezvousServerSnapshot {
    bool listening = false;
    std::string endpoint;
    std::string lastError;
    uint64_t receivedDatagrams = 0;
    uint64_t controlMessages = 0;
    RendezvousRelaySnapshot relay;
    std::vector<RendezvousRoomSnapshot> rooms;
};

class RendezvousServer {
public:
    RendezvousServer(const RendezvousConfig& config,
                     const std::atomic<bool>& running);

    int Run();
    RendezvousServerSnapshot Snapshot() const;

private:
    void UpdateSnapshot(RendezvousRegistry* registry = nullptr);

    RendezvousConfig config_;
    const std::atomic<bool>& running_;
    mutable std::mutex snapshotMutex_;
    RendezvousServerSnapshot snapshot_;
};
