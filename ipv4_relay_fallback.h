#pragma once

#include <atomic>
#include <string>

#include "config.h"
#include "util.h"

// Requests a per-pair IPv4 UDP relay from the rendezvous server and switches
// the existing IPv4 socket to that relay data plane after both peers are ready.
bool DiscoverAndConnectIpv4Relay(socket_t* sock, const Config& config,
                                 const UdpEndpoint& rendezvousServer,
                                 const std::atomic<bool>& running,
                                 const std::string& expectedPeerId,
                                 UdpEndpoint* peer, std::string* error);
