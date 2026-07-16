#include "nat4_traversal.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "log.h"
#include "nat_protocol.h"
#include "rendezvous_client.h"

namespace {
constexpr int kPunchRepeat = 5;
constexpr int kConfirmRepeat = 10;
constexpr int kDataSocketRecvTimeoutMs = 1000;

bool Send(socket_t sock, const UdpEndpoint& endpoint, const std::string& data) {
    return sendto(sock, data.data(), static_cast<int>(data.size()), 0,
        reinterpret_cast<const sockaddr*>(&endpoint.addr), endpoint.addr_len)
        == static_cast<int>(data.size());
}

UdpEndpoint FromSockaddr(const sockaddr_storage& address, socket_len_t len) {
    UdpEndpoint out{};
    out.addr = address;
    out.addr_len = len;
    out.family = address.ss_family;
    return out;
}

bool SameIpv4Address(const UdpEndpoint& a, const UdpEndpoint& b) {
    if (a.family != AF_INET || b.family != AF_INET) return false;
    const auto* aa = reinterpret_cast<const sockaddr_in*>(&a.addr);
    const auto* ba = reinterpret_cast<const sockaddr_in*>(&b.addr);
    return aa->sin_addr.s_addr == ba->sin_addr.s_addr;
}

bool WithPortOffset(const UdpEndpoint& base, uint16_t offset,
                    UdpEndpoint* endpoint) {
    if (base.family != AF_INET) return false;
    const auto* address = reinterpret_cast<const sockaddr_in*>(&base.addr);
    const uint32_t port = static_cast<uint32_t>(ntohs(address->sin_port)) + offset;
    if (port == 0 || port > 65535) return false;
    *endpoint = base;
    reinterpret_cast<sockaddr_in*>(&endpoint->addr)->sin_port =
        htons(static_cast<uint16_t>(port));
    return true;
}

void ClosePool(std::vector<socket_t>* pool, socket_t keep = kInvalidSocket) {
    for (socket_t candidate : *pool) {
        if (candidate != keep) CloseSocket(candidate);
    }
    pool->clear();
    if (keep != kInvalidSocket) pool->push_back(keep);
}

bool CreatePool(uint16_t firstPort, uint16_t count, std::vector<socket_t>* pool,
                std::string* error) {
    pool->clear();
    for (uint32_t i = 0; i < count; ++i) {
        socket_t candidate = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (candidate == kInvalidSocket) {
            *error = "Cannot create NAT4 UDP socket. err="
                + std::to_string(GetSocketError());
            ClosePool(pool);
            return false;
        }
        SetSocketRecvTimeoutMs(candidate, kDataSocketRecvTimeoutMs);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(static_cast<uint16_t>(firstPort + i));
        if (bind(candidate, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            *error = "Cannot bind NAT4 source port "
                + std::to_string(firstPort + i) + ". err="
                + std::to_string(GetSocketError());
            CloseSocket(candidate);
            ClosePool(pool);
            return false;
        }
        pool->push_back(candidate);
    }
    return true;
}

void SendPoolPunch(const std::vector<socket_t>& pool,
                   const UdpEndpoint& target, const std::string& punch,
                   int repeatCount) {
    for (int repeat = 0; repeat < repeatCount; ++repeat) {
        for (const socket_t candidate : pool) Send(candidate, target, punch);
    }
}

int WaitReadable(const std::vector<socket_t>& pool, fd_set* readable) {
    FD_ZERO(readable);
#ifndef _WIN32
    socket_t highest = 0;
#endif
    for (const socket_t candidate : pool) {
        FD_SET(candidate, readable);
#ifndef _WIN32
        highest = (std::max)(highest, candidate);
#endif
    }
    timeval timeout{};
    timeout.tv_usec = 200000;
#ifdef _WIN32
    return select(0, readable, nullptr, nullptr, &timeout);
#else
    return select(static_cast<int>(highest + 1), readable, nullptr, nullptr, &timeout);
#endif
}
}  // namespace

bool DiscoverAndPunchNat4(socket_t* sock, const Config& cfg,
                          RendezvousClient& rendezvous,
                          const std::atomic<bool>& running,
                          const std::string& expectedPeerId,
                          std::chrono::steady_clock::time_point deadline,
                          UdpEndpoint* peer, std::string* error) {
    if (cfg.nat4_source_port_count == 0) {
        *error = "NAT4 socket pool is disabled";
        return false;
    }

    uint32_t firstPort = cfg.nat4_source_port_start;
    uint32_t round = 0;
    const std::string punch = MakeControlMessage(
        "PUNCH", {cfg.room_id, cfg.peer_id});
    std::vector<uint8_t> buffer(2048);

    while (running.load() && std::chrono::steady_clock::now() < deadline) {
        if (firstPort + cfg.nat4_source_port_count - 1 > 65535) {
            *error = "NAT4 source port ranges exhausted before punch succeeded";
            return false;
        }

        std::vector<socket_t> pool;
        std::string poolError;
        if (!CreatePool(static_cast<uint16_t>(firstPort),
                        cfg.nat4_source_port_count, &pool, &poolError)) {
            Log(LogLevel::Warn, poolError + "; trying the next source-port range");
            firstPort += cfg.nat4_source_port_count;
            continue;
        }

        const socket_t registrationSocket = pool.front();
        *sock = registrationSocket;
        Log(LogLevel::Info, "NAT4 round "
            + std::to_string(round) + ", source ports "
            + std::to_string(firstPort) + "-"
            + std::to_string(firstPort + cfg.nat4_source_port_count - 1)
            + ", peer offset=+" + std::to_string(cfg.nat4_peer_port_offset));

        const auto roundDeadline = (std::min)(deadline,
            std::chrono::steady_clock::now()
                + std::chrono::seconds(cfg.nat4_round_timeout));
        auto nextRegistration = std::chrono::steady_clock::time_point{};
        auto nextPoolPunch = std::chrono::steady_clock::time_point{};
        bool havePeer = peer->family == AF_INET;
        bool punchSent = false;
        UdpEndpoint rendezvousPeer = *peer;

        while (running.load() && std::chrono::steady_clock::now() < roundDeadline) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextRegistration) {
                rendezvous.SendNat4Join(registrationSocket, expectedPeerId, round);
                nextRegistration = now + std::chrono::milliseconds(500);
            }

            fd_set readable;
            const int ready = WaitReadable(pool, &readable);
            if (ready < 0) {
                *error = "select failed during NAT4 traversal. err="
                    + std::to_string(GetSocketError());
                rendezvous.Unregister(registrationSocket);
                ClosePool(&pool);
                *sock = kInvalidSocket;
                return false;
            }
            if (ready == 0) continue;

            for (size_t candidateIndex = 0; candidateIndex < pool.size(); ++candidateIndex) {
                const socket_t candidate = pool[candidateIndex];
                if (!FD_ISSET(candidate, &readable)) continue;
                sockaddr_storage sourceAddress{};
                socket_len_t sourceLen = static_cast<socket_len_t>(sizeof(sourceAddress));
                const int n = recvfrom(candidate,
                    reinterpret_cast<char*>(buffer.data()),
                    static_cast<int>(buffer.size()), 0,
                    reinterpret_cast<sockaddr*>(&sourceAddress), &sourceLen);
                if (n < 0) continue;

                const UdpEndpoint source = FromSockaddr(sourceAddress, sourceLen);
                if (candidate == registrationSocket) {
                    const RendezvousEvent event = rendezvous.HandlePacket(
                        source, buffer.data(), static_cast<size_t>(n));
                    if (event.type == RendezvousEventType::Error) {
                        *error = event.error;
                        rendezvous.Unregister(registrationSocket);
                        ClosePool(&pool);
                        *sock = kInvalidSocket;
                        return false;
                    }
                    if (event.type == RendezvousEventType::Nat4Peer
                        && event.peerId == expectedPeerId
                        && event.round == round) {
                        const bool endpointChanged = !havePeer
                            || !SameUdpEndpoint(event.peer, rendezvousPeer);
                        rendezvousPeer = event.peer;
                        havePeer = true;
                        const auto peerInfoTime = std::chrono::steady_clock::now();
                        if (!punchSent || endpointChanged
                            || peerInfoTime >= nextPoolPunch) {
                            UdpEndpoint predicted{};
                            if (!WithPortOffset(rendezvousPeer,
                                                cfg.nat4_peer_port_offset,
                                                &predicted)) {
                                *error = "Predicted NAT4 peer port exceeds 65535";
                                rendezvous.Unregister(registrationSocket);
                                ClosePool(&pool);
                                *sock = kInvalidSocket;
                                return false;
                            }
                            Log(LogLevel::Info, "NAT4 peer observed as "
                                + FormatUdpEndpoint(rendezvousPeer) + ", target "
                                + FormatUdpEndpoint(predicted));
                            const int repeatCount = (!punchSent || endpointChanged)
                                ? kPunchRepeat : 1;
                            SendPoolPunch(pool, predicted, punch, repeatCount);
                            punchSent = true;
                            nextPoolPunch = peerInfoTime + std::chrono::seconds(2);
                        }
                    }
                    if (event.type != RendezvousEventType::None) continue;
                }

                std::string type;
                std::vector<std::string> fields;
                if (!ParseControlMessage(buffer.data(), static_cast<size_t>(n),
                                         &type, &fields)) continue;

                if (!havePeer || !SameIpv4Address(source, rendezvousPeer)
                    || fields.size() < 2 || fields[0] != cfg.room_id
                    || fields[1] != expectedPeerId
                    || (type != "PUNCH" && type != "PUNCH_ACK")) continue;

                *peer = source;
                *sock = candidate;
                Log(LogLevel::Info, "NAT4 socket pool matched local port "
                    + std::to_string(firstPort + candidateIndex)
                    + " with peer " + FormatUdpEndpoint(source));
                const std::string confirm = MakeControlMessage(
                    "PUNCH_ACK", {cfg.room_id, cfg.peer_id});
                for (int repeat = 0; repeat < kConfirmRepeat && running.load(); ++repeat) {
                    Send(candidate, source, confirm);
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                if (candidate != registrationSocket) {
                    rendezvous.Unregister(registrationSocket);
                }
                ClosePool(&pool, candidate);
                Log(LogLevel::Info, "NAT4 hole punching confirmed with "
                    + FormatUdpEndpoint(*peer));
                return true;
            }
        }

        rendezvous.Unregister(registrationSocket);
        ClosePool(&pool);
        *sock = kInvalidSocket;
        firstPort += cfg.nat4_source_port_count;
        ++round;
    }

    *error = running.load() ? "NAT4 punch timed out" : "NAT traversal stopped";
    return false;
}
