#pragma once

#include <atomic>

#include "config.h"

class RendezvousServer {
public:
    RendezvousServer(const RendezvousConfig& config,
                     const std::atomic<bool>& running);

    int Run();

private:
    const RendezvousConfig& config_;
    const std::atomic<bool>& running_;
};
