#pragma once

#include <atomic>
#include <string>

#include "config.h"
#include "util.h"

// After IPv4 traversal has failed, verifies native IPv6 reachability, exchanges
// GUA endpoints through the IPv4 rendezvous server, and confirms a direct UDP
// path. On success *sock is replaced by the IPv6 data socket.
bool DiscoverAndConnectIpv6(socket_t* sock, const Config& config,
                            const UdpEndpoint& rendezvousServer,
                            const std::atomic<bool>& running,
                            const std::string& expectedPeerId,
                            UdpEndpoint* peer, std::string* error);
