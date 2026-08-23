#pragma once

#include <atomic>
#include <string>

#include "config.h"
#include "rendezvous_client.h"
#include "util.h"

// Runs the STUN-based adaptive NAT punch on a dedicated UDP socket. The
// existing rendezvous socket is replaced only after the punch succeeds; on
// failure it remains available to subsequent IPv6/relay strategies.
bool PunchAdaptiveNat(socket_t* sock, const Config& cfg,
                      const UdpEndpoint& server,
                      const std::atomic<bool>& running,
                      const std::string& matchedPeerId,
                      const NatPunchSession& session,
                      UdpEndpoint* peer, std::string* error);

