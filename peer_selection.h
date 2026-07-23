#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "config.h"
#include "util.h"

// Registers with the rendezvous server and selects a peer before any
// traversal strategy is attempted.
bool SelectPeer(socket_t sock, const Config& config,
                const UdpEndpoint& server,
                const std::atomic<bool>& running,
                UdpEndpoint* peer, std::string* matchedPeerId,
                std::vector<TraversalMode>* traversalModes,
                std::string* error);
