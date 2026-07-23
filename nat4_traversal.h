#pragma once

#include <atomic>
#include <string>

#include "config.h"
#include "util.h"

// Runs the n4-style traversal strategy. It replaces *sock with the socket that receives
// the peer's punch; all other sockets in the winning pool are closed.
bool DiscoverAndPunchNat4(socket_t* sock, const Config& cfg,
                          const UdpEndpoint& rendezvousServer,
                          const std::atomic<bool>& running,
                          const std::string& expectedPeerId,
                          UdpEndpoint* peer, std::string* error);
