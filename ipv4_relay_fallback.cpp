#include "ipv4_relay_fallback.h"

#include <chrono>
#include <cstdint>
#include <vector>

#include "log.h"
#include "nat_protocol.h"

namespace {
constexpr int kRelayReceiveTimeoutMs = 200;

bool Send(socket_t sock, const UdpEndpoint& endpoint, const std::string& data) {
    return sendto(sock, data.data(), static_cast<int>(data.size()), 0,
        reinterpret_cast<const sockaddr*>(&endpoint.addr), endpoint.addr_len)
        == static_cast<int>(data.size());
}

UdpEndpoint FromSockaddr(const sockaddr_storage& address, socket_len_t len) {
    UdpEndpoint endpoint{};
    endpoint.addr = address;
    endpoint.addr_len = len;
    endpoint.family = address.ss_family;
    return endpoint;
}

bool ParsePort(const std::string& text, uint16_t* port) {
    try {
        size_t consumed = 0;
        const unsigned long parsed = std::stoul(text, &consumed);
        if (consumed != text.size() || parsed == 0 || parsed > 65535) return false;
        *port = static_cast<uint16_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

UdpEndpoint WithPort(const UdpEndpoint& endpoint, uint16_t port) {
    UdpEndpoint result = endpoint;
    reinterpret_cast<sockaddr_in*>(&result.addr)->sin_port = htons(port);
    return result;
}
}  // namespace

bool DiscoverAndConnectIpv4Relay(
    socket_t* sock, const Config& config,
    const UdpEndpoint& rendezvousServer, const std::atomic<bool>& running,
    const std::string& expectedPeerId, UdpEndpoint* peer, std::string* error) {
    if (rendezvousServer.family != AF_INET || expectedPeerId.empty()) {
        *error = "IPv4 relay requires a matched peer and an IPv4 rendezvous server";
        return false;
    }

    if (*sock == kInvalidSocket) {
        *sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (*sock == kInvalidSocket) {
            *error = "Cannot create IPv4 relay UDP socket. err="
                + std::to_string(GetSocketError());
            return false;
        }
    }
    SetSocketRecvTimeoutMs(*sock, kRelayReceiveTimeoutMs);
    // Keep cleanup on the IPv4 rendezvous path even if a failed IPv6 attempt
    // previously left an AF_INET6 candidate in *peer.
    *peer = rendezvousServer;

    const std::string join = MakeControlMessage("RELAY_JOIN",
        {config.room_id, config.peer_id, expectedPeerId, config.auth_token});
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(config.punch_timeout);
    auto nextJoin = std::chrono::steady_clock::time_point{};
    auto nextHello = std::chrono::steady_clock::time_point{};
    UdpEndpoint relayEndpoint{};
    std::string sessionId;
    std::string accessKey;
    bool haveOffer = false;
    std::vector<uint8_t> buffer(2048);

    while (running.load() && std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextJoin) {
            Send(*sock, rendezvousServer, join);
            nextJoin = now + std::chrono::milliseconds(haveOffer ? 1000 : 500);
        }
        if (haveOffer && now >= nextHello) {
            Send(*sock, relayEndpoint, MakeControlMessage("RELAY_HELLO",
                {sessionId, config.peer_id, accessKey}));
            nextHello = now + std::chrono::milliseconds(500);
        }

        sockaddr_storage sourceAddress{};
        socket_len_t sourceLen = static_cast<socket_len_t>(sizeof(sourceAddress));
        const int received = recvfrom(*sock,
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&sourceAddress), &sourceLen);
        if (received < 0) {
            if (!running.load()) break;
            const int receiveError = GetSocketError();
            if (IsRecvTimeout(receiveError)
                || IsUdpDestinationUnreachable(receiveError)) continue;
            *error = "IPv4 relay receive failed. err="
                + std::to_string(receiveError);
            return false;
        }

        const UdpEndpoint source = FromSockaddr(sourceAddress, sourceLen);
        std::string type;
        std::vector<std::string> fields;
        if (!ParseControlMessage(buffer.data(), static_cast<size_t>(received),
                                 &type, &fields)) continue;

        if (SameUdpEndpoint(source, rendezvousServer)) {
            if (type == "ERROR") {
                *error = fields.empty() ? "IPv4 relay rejected by rendezvous"
                    : "IPv4 relay: " + fields[0];
                return false;
            }
            if (type != "RELAY_OFFER" || fields.size() != 4
                || fields[1] != expectedPeerId
                || !IsSafeControlField(fields[2])
                || !IsSafeControlField(fields[3])) continue;
            uint16_t relayPort = 0;
            if (!ParsePort(fields[0], &relayPort)) continue;
            relayEndpoint = WithPort(rendezvousServer, relayPort);
            sessionId = fields[2];
            accessKey = fields[3];
            if (!haveOffer) {
                Log(LogLevel::Info, "IPv4 relay offered for peer "
                    + expectedPeerId + " at "
                    + FormatUdpEndpoint(relayEndpoint));
            }
            haveOffer = true;
            nextHello = std::chrono::steady_clock::time_point{};
            continue;
        }

        if (!haveOffer || !SameUdpEndpoint(source, relayEndpoint)
            || type != "RELAY_READY" || fields.size() != 2
            || fields[0] != sessionId || fields[1] != expectedPeerId) continue;

        *peer = relayEndpoint;
        Log(LogLevel::Info, "IPv4 relay fallback confirmed with "
            + FormatUdpEndpoint(*peer));
        return true;
    }

    *error = running.load() ? "IPv4 relay fallback timed out"
                            : "IPv4 relay fallback stopped";
    return false;
}
