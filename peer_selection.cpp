#include "peer_selection.h"

#include <chrono>
#include <vector>

#include "log.h"
#include "rendezvous_client.h"

namespace {
UdpEndpoint FromSockaddr(const sockaddr_storage& address, socket_len_t len) {
    UdpEndpoint endpoint{};
    endpoint.addr = address;
    endpoint.addr_len = len;
    endpoint.family = address.ss_family;
    return endpoint;
}

const char* RendezvousEventName(RendezvousEventType type) {
    switch (type) {
        case RendezvousEventType::None: return "None";
        case RendezvousEventType::Registered: return "Registered";
        case RendezvousEventType::PeerUnavailable: return "PeerUnavailable";
        case RendezvousEventType::Peer: return "Peer";
        case RendezvousEventType::Nat4Wait: return "Nat4Wait";
        case RendezvousEventType::Nat4Peer: return "Nat4Peer";
        case RendezvousEventType::Error: return "Error";
        default: return "Unknown";
    }
}

std::string LocalSocketEndpoint(socket_t sock) {
    sockaddr_storage address{};
    socket_len_t len = static_cast<socket_len_t>(sizeof(address));
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&address), &len) != 0) {
        return "unknown (getsockname err=" + std::to_string(GetSocketError()) + ")";
    }
    return FormatUdpEndpoint(FromSockaddr(address, len));
}

long long MillisecondsRemaining(
    std::chrono::steady_clock::time_point deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    return remaining > 0 ? static_cast<long long>(remaining) : 0LL;
}
}  // namespace

bool SelectPeer(socket_t sock, const Config& config,
                const UdpEndpoint& server,
                const std::atomic<bool>& running,
                UdpEndpoint* peer, std::string* matchedPeerId,
                std::string* error) {
    if (!ValidateRendezvousSession(config, error)) return false;

    RendezvousClient rendezvous(config, server);
    const auto selectionDeadline = std::chrono::steady_clock::now()
        + (config.target_peer_id.empty()
               ? std::chrono::hours(24 * 365)
               : std::chrono::seconds(config.punch_timeout));
    auto nextProbe = std::chrono::steady_clock::time_point{};
    std::vector<uint8_t> buffer(2048);

    Log(LogLevel::Debug, "Peer selection started: mode="
        + std::string(config.target_peer_id.empty() ? "wait" : "connect")
        + ", peer_id=" + config.peer_id
        + (config.target_peer_id.empty()
               ? "" : ", target_peer_id=" + config.target_peer_id)
        + ", selection_timeout_ms="
        + std::to_string(
            static_cast<unsigned long long>(config.punch_timeout) * 1000ULL));

    while (running.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (rendezvous.ResponseTimedOut(now, error)) {
            Log(LogLevel::Error, "Peer selection aborted: " + *error);
            return false;
        }
        if (rendezvous.HasResponded() && now >= selectionDeadline) break;

        if (now >= nextProbe) {
            if (!rendezvous.SendProbe(sock)) {
                Log(LogLevel::Warn, "Failed to send rendezvous probe to "
                    + FormatUdpEndpoint(server) + ". err="
                    + std::to_string(GetSocketError()));
            } else {
                Log(LogLevel::Debug, "Sent rendezvous probe; local="
                    + LocalSocketEndpoint(sock) + ", server="
                    + FormatUdpEndpoint(server));
            }
            nextProbe = now + std::chrono::milliseconds(500);
        }

        sockaddr_storage sourceAddress{};
        socket_len_t sourceLen =
            static_cast<socket_len_t>(sizeof(sourceAddress));
        const int received = recvfrom(
            sock, reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&sourceAddress), &sourceLen);
        if (received < 0) {
            const int socketError = GetSocketError();
            if (IsRecvTimeout(socketError)) continue;
            if (rendezvous.HandleUnreachableError(socketError, error)) {
                Log(LogLevel::Error,
                    "Peer selection failed before rendezvous response: "
                    + *error + ". err=" + std::to_string(socketError));
                return false;
            }
            if (IsUdpDestinationUnreachable(socketError)) {
                Log(LogLevel::Debug,
                    "Transient UDP unreachable during peer selection; "
                    "continuing. err=" + std::to_string(socketError)
                    + ", deadline_ms="
                    + std::to_string(
                        MillisecondsRemaining(selectionDeadline)));
                continue;
            }
            *error = "UDP receive failed during peer selection. err="
                + std::to_string(socketError);
            Log(LogLevel::Error, *error);
            return false;
        }

        const UdpEndpoint source = FromSockaddr(sourceAddress, sourceLen);
        const RendezvousEvent event = rendezvous.HandlePacket(
            source, buffer.data(), static_cast<size_t>(received));
        Log(LogLevel::Debug, "Peer selection RX: source="
            + FormatUdpEndpoint(source)
            + ", bytes=" + std::to_string(received)
            + ", rendezvous_event=" + RendezvousEventName(event.type));

        if (event.type == RendezvousEventType::Error) {
            *error = event.error;
            Log(LogLevel::Error, "Rendezvous packet rejected: " + *error);
            return false;
        }
        if (event.type == RendezvousEventType::Peer
            && (config.target_peer_id.empty()
                || event.peerId == config.target_peer_id)) {
            *peer = event.peer;
            *matchedPeerId = event.peerId;
            Log(LogLevel::Info, "Rendezvous selected peer id="
                + *matchedPeerId + ", endpoint=" + FormatUdpEndpoint(*peer));
            return true;
        }
    }

    *error = running.load() ? "Timed out waiting for selected peer"
                            : "Peer selection stopped by request";
    Log(running.load() ? LogLevel::Error : LogLevel::Debug,
        "Peer selection finished without a peer: " + *error);
    return false;
}
