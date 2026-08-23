#include "nat_punch_transport.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace {
bool SetIpv4SocketTtl(socket_t sock, int ttl, int* socketError) {
    if (setsockopt(sock, IPPROTO_IP, IP_TTL,
                   reinterpret_cast<const char*>(&ttl),
                   static_cast<socket_len_t>(sizeof(ttl))) == 0) {
        return true;
    }
    if (socketError != nullptr) *socketError = GetSocketError();
    return false;
}
}  // namespace

ScopedIpv4SocketTtl::ScopedIpv4SocketTtl(
        socket_t sock, uint8_t ttl, int* socketError)
    : sock_(sock) {
    if (socketError != nullptr) *socketError = 0;
    if (sock_ == kInvalidSocket || ttl == 0) return;
    socket_len_t optionLength =
        static_cast<socket_len_t>(sizeof(originalTtl_));
    if (getsockopt(sock_, IPPROTO_IP, IP_TTL,
                   reinterpret_cast<char*>(&originalTtl_),
                   &optionLength) != 0) {
        if (socketError != nullptr) *socketError = GetSocketError();
        return;
    }
    applied_ = SetIpv4SocketTtl(
        sock_, static_cast<int>(ttl), socketError);
}

ScopedIpv4SocketTtl::~ScopedIpv4SocketTtl() {
    Restore(nullptr);
}

bool ScopedIpv4SocketTtl::Restore(int* socketError) {
    if (socketError != nullptr) *socketError = 0;
    if (!applied_) return true;
    if (!SetIpv4SocketTtl(sock_, originalTtl_, socketError)) return false;
    applied_ = false;
    return true;
}

bool WaitForNatPunchDelay(const std::atomic<bool>& running,
                          uint32_t delayMs) {
    const auto delayDeadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(delayMs);
    while (running.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= delayDeadline) return true;
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(delayDeadline - now);
        std::this_thread::sleep_for((std::min)(
            remaining, std::chrono::milliseconds(25)));
    }
    return false;
}
