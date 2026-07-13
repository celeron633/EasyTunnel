#pragma once

#include <atomic>
#include <chrono>
#include <string>

#include "config.h"
#include "util.h"

// Runs the n4-style fallback. It replaces *sock with the socket that receives
// the peer's punch; all other sockets in the winning pool are closed.
bool DiscoverAndPunchNat4(socket_t* sock, const Config& cfg,
                          const UdpEndpoint& server,
                          const std::atomic<bool>& running,
                          const std::string& expectedPeerId,
                          std::chrono::steady_clock::time_point deadline,
                          UdpEndpoint* peer, std::string* error);
