#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "util.h"

// Temporarily applies an IPv4 unicast TTL to one UDP socket. Call Restore
// before handing a socket to the data plane; the destructor is a best-effort
// fallback for early returns.
class ScopedIpv4SocketTtl {
public:
    ScopedIpv4SocketTtl(socket_t sock, uint8_t ttl, int* socketError);
    ~ScopedIpv4SocketTtl();

    ScopedIpv4SocketTtl(const ScopedIpv4SocketTtl&) = delete;
    ScopedIpv4SocketTtl& operator=(const ScopedIpv4SocketTtl&) = delete;

    bool applied() const { return applied_; }
    bool Restore(int* socketError);

private:
    socket_t sock_ = kInvalidSocket;
    int originalTtl_ = 0;
    bool applied_ = false;
};

struct NatPunchSendResult {
    bool sent = false;
    bool ttlApplied = false;
    bool ttlRestored = true;
    int socketError = 0;
};

using NatPunchDatagramSender = bool (*)(
    socket_t, const UdpEndpoint&, const std::string&);

// A failed low-TTL application skips the probe, never silently sends it with
// ordinary TTL. All paths restore TTL before an ACK or data-plane handoff.
NatPunchSendResult SendNatPunchProbe(
    socket_t socket, const UdpEndpoint& target, const std::string& packet,
    uint8_t ttl, NatPunchDatagramSender sender = nullptr);

// Returns false when shutdown interrupts the delay.
bool WaitForNatPunchDelay(const std::atomic<bool>& running,
                          uint32_t delayMs);
