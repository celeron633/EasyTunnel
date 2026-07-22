#include "ipv6_fallback.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include "log.h"
#include "nat_protocol.h"
#include "rendezvous_client.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
constexpr int kProbeTimeoutMs = 3000;
constexpr int kSelectIntervalMs = 200;

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

bool IsIpv6Gua(const in6_addr& address) {
    // IANA global-unicast block 2000::/3. The external probe rejects unrouted
    // addresses in practice.
    return (address.s6_addr[0] & 0xe0) == 0x20;
}

bool SetNonBlocking(socket_t sock, bool enabled) {
#ifdef _WIN32
    u_long mode = enabled ? 1UL : 0UL;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(sock, F_GETFL, 0);
    return flags >= 0 && fcntl(sock, F_SETFL,
        enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
}

bool ConnectInProgress(int error) {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS
        || error == WSAEINVAL;
#else
    return error == EINPROGRESS || error == EWOULDBLOCK;
#endif
}

bool ProbeOne(const sockaddr_in6& remote, in6_addr* localAddress) {
    socket_t probe = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (probe == kInvalidSocket) return false;
    if (!SetNonBlocking(probe, true)) {
        CloseSocket(probe);
        return false;
    }

    const int result = connect(probe, reinterpret_cast<const sockaddr*>(&remote),
                               sizeof(remote));
    if (result != 0 && !ConnectInProgress(GetSocketError())) {
        CloseSocket(probe);
        return false;
    }
    if (result != 0) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(probe, &writable);
        timeval timeout{};
        timeout.tv_sec = kProbeTimeoutMs / 1000;
        timeout.tv_usec = (kProbeTimeoutMs % 1000) * 1000;
#ifdef _WIN32
        const int ready = select(0, nullptr, &writable, nullptr, &timeout);
#else
        const int ready = select(probe + 1, nullptr, &writable, nullptr, &timeout);
#endif
        int connectError = 0;
        socket_len_t errorLen = static_cast<socket_len_t>(sizeof(connectError));
        if (ready <= 0 || getsockopt(probe, SOL_SOCKET, SO_ERROR,
                reinterpret_cast<char*>(&connectError), &errorLen) != 0
            || connectError != 0) {
            CloseSocket(probe);
            return false;
        }
    }

    sockaddr_in6 local{};
    socket_len_t localLen = static_cast<socket_len_t>(sizeof(local));
    const bool ok = getsockname(probe, reinterpret_cast<sockaddr*>(&local),
                                &localLen) == 0
        && local.sin6_family == AF_INET6 && IsIpv6Gua(local.sin6_addr);
    if (ok) *localAddress = local.sin6_addr;
    CloseSocket(probe);
    return ok;
}

bool DiscoverReachableGua(const std::string& probeHost, uint16_t probePort,
                          in6_addr* address, std::string* text) {
    addrinfo hints{};
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    const std::string service = std::to_string(probePort);
    if (getaddrinfo(probeHost.c_str(), service.c_str(), &hints, &results) != 0
        || results == nullptr) {
        if (results != nullptr) freeaddrinfo(results);
        return false;
    }
    bool reachable = false;
    for (const addrinfo* result = results; result != nullptr;
         result = result->ai_next) {
        if (result->ai_family != AF_INET6
            || static_cast<size_t>(result->ai_addrlen) < sizeof(sockaddr_in6)) {
            continue;
        }
        sockaddr_in6 remote{};
        std::memcpy(&remote, result->ai_addr, sizeof(remote));
        if (!ProbeOne(remote, address)) continue;
        char buffer[INET6_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET6, address, buffer, sizeof(buffer)) == nullptr) {
            break;
        }
        *text = buffer;
        reachable = true;
        break;
    }
    freeaddrinfo(results);
    return reachable;
}

bool ParsePort(const std::string& text, uint16_t* port) {
    try {
        size_t consumed = 0;
        const unsigned long value = std::stoul(text, &consumed);
        if (consumed != text.size() || value == 0 || value > 65535) return false;
        *port = static_cast<uint16_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

int WaitReadable(socket_t first, socket_t second, fd_set* readable) {
    FD_ZERO(readable);
    FD_SET(first, readable);
    FD_SET(second, readable);
    timeval timeout{};
    timeout.tv_usec = kSelectIntervalMs * 1000;
#ifdef _WIN32
    return select(0, readable, nullptr, nullptr, &timeout);
#else
    return select(static_cast<int>((std::max)(first, second) + 1),
                  readable, nullptr, nullptr, &timeout);
#endif
}

void Unregister(socket_t control, const Config& config,
                const UdpEndpoint& server) {
    Send(control, server, MakeControlMessage("UNREG",
        {config.room_id, config.peer_id, config.auth_token}));
}
}  // namespace

bool DiscoverAndConnectIpv6(socket_t* sock, const Config& config,
                            const UdpEndpoint& rendezvousServer,
                            const std::atomic<bool>& running,
                            const std::string& expectedPeerId,
                            UdpEndpoint* peer, std::string* error) {
    if (!config.ipv6_fallback_enabled) {
        *error = "IPv6 fallback is disabled";
        return false;
    }
    if (expectedPeerId.empty()) {
        *error = "IPv6 fallback has no matched peer";
        return false;
    }

    in6_addr localAddress{};
    std::string localAddressText;
    Log(LogLevel::Info, "IPv4 traversal failed; checking IPv6 fallback");
    if (!DiscoverReachableGua(config.ipv6_probe_host, config.ipv6_probe_port,
                              &localAddress, &localAddressText)) {
        *error = "No reachable IPv6 GUA (TCP probe to "
            + config.ipv6_probe_host + ":"
            + std::to_string(config.ipv6_probe_port) + " failed)";
        return false;
    }
    Log(LogLevel::Info, "IPv6 GUA connectivity verified via "
        + config.ipv6_probe_host + ":"
        + std::to_string(config.ipv6_probe_port) + ": " + localAddressText);

    socket_t dataSocket = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (dataSocket == kInvalidSocket) {
        *error = "Cannot create IPv6 UDP socket. err="
            + std::to_string(GetSocketError());
        return false;
    }
    int v6Only = 1;
    setsockopt(dataSocket, IPPROTO_IPV6, IPV6_V6ONLY,
               reinterpret_cast<const char*>(&v6Only), sizeof(v6Only));
    sockaddr_in6 local{};
    local.sin6_family = AF_INET6;
    local.sin6_addr = localAddress;
    local.sin6_port = htons(config.ipv6_listen_port);
    if (bind(dataSocket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        *error = "Cannot bind IPv6 fallback socket. err="
            + std::to_string(GetSocketError());
        CloseSocket(dataSocket);
        return false;
    }
    SetSocketRecvTimeoutMs(dataSocket, 1000);
    socket_len_t localLen = static_cast<socket_len_t>(sizeof(local));
    if (getsockname(dataSocket, reinterpret_cast<sockaddr*>(&local), &localLen) != 0) {
        *error = "Cannot read IPv6 fallback port. err="
            + std::to_string(GetSocketError());
        CloseSocket(dataSocket);
        return false;
    }
    const uint16_t localPort = ntohs(local.sin6_port);

    socket_t controlSocket = kInvalidSocket;
    UdpEndpoint controlServer{};
    std::string controlError;
    if (!OpenRendezvousSocket(config, kSelectIntervalMs, &controlSocket,
                              &controlServer, &controlError)) {
        *error = "Cannot open IPv6 fallback control channel: " + controlError;
        CloseSocket(dataSocket);
        return false;
    }
    if (rendezvousServer.family == AF_INET) controlServer = rendezvousServer;

    const std::string join = MakeControlMessage("V6_JOIN",
        {config.room_id, config.peer_id, expectedPeerId, localAddressText,
         std::to_string(localPort), config.ipv6_accept_inbound ? "1" : "0",
         config.auth_token});
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(config.ipv6_fallback_timeout);
    auto nextJoin = std::chrono::steady_clock::time_point{};
    auto nextPunch = std::chrono::steady_clock::time_point{};
    bool havePeer = false;
    bool connector = false;
    std::vector<uint8_t> buffer(2048);

    while (running.load() && std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextJoin) {
            Send(controlSocket, controlServer, join);
            nextJoin = now + std::chrono::milliseconds(500);
        }
        if (havePeer && connector && now >= nextPunch) {
            Send(dataSocket, *peer, MakeControlMessage(
                "V6_PUNCH", {config.room_id, config.peer_id}));
            nextPunch = now + std::chrono::milliseconds(200);
        }

        fd_set readable;
        const int ready = WaitReadable(controlSocket, dataSocket, &readable);
        if (ready < 0) {
            *error = "select failed during IPv6 fallback. err="
                + std::to_string(GetSocketError());
            break;
        }
        if (ready == 0) continue;

        for (socket_t sourceSocket : {controlSocket, dataSocket}) {
            if (!FD_ISSET(sourceSocket, &readable)) continue;
            sockaddr_storage sourceAddress{};
            socket_len_t sourceLen = static_cast<socket_len_t>(sizeof(sourceAddress));
            const int received = recvfrom(sourceSocket,
                reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()), 0,
                reinterpret_cast<sockaddr*>(&sourceAddress), &sourceLen);
            if (received < 0) continue;
            const UdpEndpoint source = FromSockaddr(sourceAddress, sourceLen);
            std::string type;
            std::vector<std::string> fields;
            if (!ParseControlMessage(buffer.data(), static_cast<size_t>(received),
                                     &type, &fields)) continue;

            if (sourceSocket == controlSocket) {
                if (!SameUdpEndpoint(source, controlServer)) continue;
                if (type == "ERROR") {
                    *error = fields.empty() ? "IPv6 fallback rejected by rendezvous"
                                            : "IPv6 fallback: " + fields[0];
                    Unregister(controlSocket, config, controlServer);
                    CloseSocket(controlSocket);
                    CloseSocket(dataSocket);
                    return false;
                }
                if (type != "V6_PEER" || fields.size() != 4
                    || fields[2] != expectedPeerId
                    || (fields[3] != "connect" && fields[3] != "listen")) {
                    continue;
                }
                uint16_t peerPort = 0;
                UdpEndpoint candidate{};
                if (!ParsePort(fields[1], &peerPort)
                    || !ParseUdpEndpoint(fields[0], peerPort, &candidate)
                    || candidate.family != AF_INET6) continue;
                *peer = candidate;
                connector = fields[3] == "connect";
                havePeer = true;
                Log(LogLevel::Info, "IPv6 peer " + expectedPeerId + " at "
                    + FormatUdpEndpoint(*peer) + ", role=" + fields[3]);
                continue;
            }

            if (!havePeer || !SameUdpEndpoint(source, *peer)
                || fields.size() < 2 || fields[0] != config.room_id
                || fields[1] != expectedPeerId
                || (type != "V6_PUNCH" && type != "V6_PUNCH_ACK")) {
                continue;
            }
            if (type == "V6_PUNCH") {
                const std::string ack = MakeControlMessage(
                    "V6_PUNCH_ACK", {config.room_id, config.peer_id});
                for (int repeat = 0; repeat < 5; ++repeat) {
                    Send(dataSocket, *peer, ack);
                }
            }
            Unregister(controlSocket, config, controlServer);
            CloseSocket(controlSocket);
            if (*sock != kInvalidSocket) CloseSocket(*sock);
            *sock = dataSocket;
            Log(LogLevel::Info, "IPv6 fallback confirmed with "
                + FormatUdpEndpoint(*peer));
            return true;
        }
    }

    Unregister(controlSocket, config, controlServer);
    CloseSocket(controlSocket);
    CloseSocket(dataSocket);
    if (!running.load()) *error = "IPv6 fallback stopped";
    else if (error->empty()) *error = "IPv6 fallback timed out";
    return false;
}
