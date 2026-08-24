#include "registry.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>

#include "../log.h"
#include "../nat_protocol.h"
#include "../secure_random.h"
#include "ipv4_relay_app.h"

namespace {
struct NatPunchInfo {
    bool reported = false;
    bool armed = false;
    std::string behavior;
    UdpEndpoint mappedA{};
    UdpEndpoint mappedB{};
    std::string localCandidates;
};

struct Client {
    std::string node;
    UdpEndpoint endpoint;
    std::chrono::steady_clock::time_point seen;
    std::string tunIp;
    std::string pairedWith;
    std::string ipv6Address;
    uint16_t ipv6Port = 0;
    bool ipv6AcceptInbound = false;
    bool ipv6Joined = false;
    std::vector<TraversalMode> capabilities;
    std::vector<TraversalMode> negotiatedModes;
    std::string pairInitiator;
    std::string natSessionId;
    std::string natPunchToken;
    uint64_t natAttemptId = 0;
    bool natRetryRequested = false;
    NatPunchInfo natPunch;
};
using Room = std::unordered_map<std::string, Client>;

void ResetPairing(Client* client) {
    client->pairedWith.clear();
    client->negotiatedModes.clear();
    client->pairInitiator.clear();
    client->natSessionId.clear();
    client->natPunchToken.clear();
    client->natAttemptId = 0;
    client->natRetryRequested = false;
    client->natPunch = {};
    client->ipv6Joined = false;
}

bool Send(socket_t sock, const UdpEndpoint& endpoint, const std::string& data) {
    return sendto(sock, data.data(), static_cast<int>(data.size()), 0,
        reinterpret_cast<const sockaddr*>(&endpoint.addr), endpoint.addr_len)
        == static_cast<int>(data.size());
}

bool SendMessage(socket_t sock, const UdpEndpoint& endpoint,
                 const std::string& type,
                 const std::vector<std::string>& fields = {}) {
    return Send(sock, endpoint, MakeControlMessage(type, fields));
}

void SendProtocolVersionMismatch(socket_t sock, const UdpEndpoint& endpoint,
                                 const std::string& received) {
    const std::string safeReceived = IsSafeControlField(received)
        ? received : received.empty() ? "legacy" : "invalid";
    SendMessage(sock, endpoint, "ERROR",
        {"nat-punch-version-mismatch", kNatPunchProtocolVersion,
         safeReceived});
}

std::pair<std::string, std::string> IpAndPort(const UdpEndpoint& endpoint) {
    const auto* address = reinterpret_cast<const sockaddr_in*>(&endpoint.addr);
    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &address->sin_addr, ip, sizeof(ip));
    return {ip, std::to_string(ntohs(address->sin_port))};
}

size_t RemoveExpired(Room* room, std::chrono::steady_clock::time_point now,
                     uint16_t timeoutSeconds) {
    size_t removed = 0;
    for (auto it = room->begin(); it != room->end();) {
        if (now - it->second.seen > std::chrono::seconds(timeoutSeconds)) {
            it = room->erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    for (auto& entry : *room) {
        if (!entry.second.pairedWith.empty()
            && room->find(entry.second.pairedWith) == room->end()) {
            ResetPairing(&entry.second);
        }
    }
    return removed;
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

bool ParseAttemptId(const std::string& text, uint64_t* attemptId) {
    try {
        size_t consumed = 0;
        const unsigned long long parsed = std::stoull(text, &consumed);
        if (consumed != text.size() || parsed == 0) return false;
        *attemptId = static_cast<uint64_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool IsNatMappingBehavior(const std::string& behavior) {
    return behavior == "endpoint-independent"
        || behavior == "port-dependent-regular"
        || behavior == "port-dependent-random"
        || behavior == "multi-public-ip"
        || behavior == "unknown";
}

bool IsIpv6Gua(const in6_addr& address) {
    return (address.s6_addr[0] & 0xe0) == 0x20;
}

// Fields per client in a CLIENTS reply: node, endpoint, capabilities,
// TUN IP and idle seconds.
constexpr size_t kClientFields = 5;
// Keeps a CLIENTS datagram below the 8192-byte control message limit even
// when a room holds the maximum number of clients with long node IDs.
constexpr size_t kMaxClientListBytes = 7168;
constexpr const char* kUnknownField = "-";
}  // namespace

struct RendezvousRegistry::Impl {
    Impl(socket_t socket, const RendezvousConfig& settings)
        : sock(socket), config(settings), relayApp(socket, settings) {}

    socket_t sock;
    RendezvousConfig config;
    Ipv4RelayApp relayApp;
    std::unordered_map<std::string, Room> rooms;
    uint64_t nextNatAttemptId = 1;

    bool Authorized(const std::string& roomId, const std::string& nodeId,
                    const std::string& targetId, const std::string& token) const {
        return IsSafeControlField(roomId) && IsSafeControlField(nodeId)
            && (targetId.empty() || IsSafeControlField(targetId))
            && (token.empty() || IsSafeControlField(token))
            && (config.authToken.empty() || token == config.authToken);
    }

    // Returns the clients of a room that are not paired yet, sorted by node ID.
    std::vector<const Client*> AvailableClients(
        const std::string& roomId, std::chrono::steady_clock::time_point now) {
        std::vector<const Client*> available;
        const auto roomIt = rooms.find(roomId);
        if (roomIt == rooms.end()) return available;
        const size_t expired = RemoveExpired(&roomIt->second, now,
            config.clientTimeoutSeconds);
        if (expired > 0) {
            Log(LogLevel::Info, "Expired " + std::to_string(expired)
                + " client(s) from room=" + roomId);
        }
        for (const auto& entry : roomIt->second) {
            if (entry.second.pairedWith.empty()) available.push_back(&entry.second);
        }
        std::sort(available.begin(), available.end(),
                  [](const Client* left, const Client* right) {
                      return left->node < right->node;
                  });
        return available;
    }

    // Every client is described by kClientFields consecutive fields so
    // the UI can show its public endpoint, negotiable capabilities, TUN IP and
    // idle time next to the node ID.
    void HandleList(const UdpEndpoint& source, const std::vector<std::string>& fields,
                    std::chrono::steady_clock::time_point now) {
        if (fields.size() != 2 || !IsSafeControlField(fields[0])
            || (!fields[1].empty() && !IsSafeControlField(fields[1]))
            || (!config.authToken.empty() && fields[1] != config.authToken)) {
            Log(LogLevel::Warn, "Rejected LIST from " + FormatUdpEndpoint(source));
            SendMessage(sock, source, "ERROR", {"unauthorized"});
            return;
        }
        const std::vector<const Client*> available =
            AvailableClients(fields[0], now);
        std::vector<std::string> entries;
        size_t used = std::string("ETN1\tCLIENTS").size();
        size_t reported = 0;
        for (const Client* client : available) {
            const auto idle = std::chrono::duration_cast<std::chrono::seconds>(
                now - client->seen).count();
            const std::string record[kClientFields] = {
                client->node,
                FormatUdpEndpoint(client->endpoint),
                SerializeTraversalModeSequence(client->capabilities),
                client->tunIp.empty() ? kUnknownField : client->tunIp,
                std::to_string(static_cast<uint64_t>((std::max)(int64_t{0}, idle))),
            };
            size_t needed = 0;
            for (const auto& field : record) needed += 1 + field.size();
            if (used + needed > kMaxClientListBytes) break;
            used += needed;
            entries.insert(entries.end(), std::begin(record), std::end(record));
            ++reported;
        }
        if (reported < available.size()) {
            Log(LogLevel::Warn, "Truncated LIST room=" + fields[0] + " reported="
                + std::to_string(reported) + " available="
                + std::to_string(available.size()));
        }
        Log(LogLevel::Debug, "LIST room=" + fields[0] + " clients="
            + std::to_string(reported) + " source=" + FormatUdpEndpoint(source));
        SendMessage(sock, source, "CLIENTS", entries);
    }

    bool ValidateNatPair(Room& room, Room::iterator current,
                         const std::string& targetId,
                         const std::string& sessionId,
                         Room::iterator* target) {
        *target = room.find(targetId);
        return *target != room.end() && *target != current
            && current->second.pairedWith == targetId
            && (*target)->second.pairedWith == current->first
            && current->second.natSessionId == sessionId
            && (*target)->second.natSessionId == sessionId;
    }

    bool ValidateNatSession(Room& room, Room::iterator current,
                            const std::string& targetId,
                            const std::string& sessionId,
                            uint64_t attemptId, Room::iterator* target) {
        return ValidateNatPair(room, current, targetId, sessionId, target)
            && current->second.natAttemptId == attemptId
            && (*target)->second.natAttemptId == attemptId;
    }

    std::vector<std::string> NatAttemptFields(const Client& client) const {
        return {
            client.natSessionId,
            std::to_string(client.natAttemptId),
            client.node == client.pairInitiator ? "initiator" : "responder",
            kNatPunchProtocolVersion,
            client.natPunchToken,
        };
    }

    void SendNatAttempt(const Client& client) {
        SendMessage(sock, client.endpoint, "NAT_ATTEMPT",
                    NatAttemptFields(client));
    }

    std::vector<std::string> NatPeerInfoFields(
        const Client& recipient, const Client& peer) const {
        const auto mappedA = IpAndPort(peer.natPunch.mappedA);
        const auto mappedB = IpAndPort(peer.natPunch.mappedB);
        return {
            recipient.natSessionId,
            std::to_string(recipient.natAttemptId),
            peer.node,
            peer.natPunch.behavior,
            mappedA.first,
            mappedA.second,
            mappedB.first,
            mappedB.second,
            peer.natPunch.localCandidates,
            recipient.node == recipient.pairInitiator
                ? "initiator" : "responder",
            kNatPunchProtocolVersion,
        };
    }

    void SendNatPeerInfo(Client* first, Client* second) {
        if (!SendMessage(sock, first->endpoint, "NAT_PEER_INFO",
                         NatPeerInfoFields(*first, *second))) {
            Log(LogLevel::Warn, "Failed to send NAT_PEER_INFO to peer="
                + first->node + " control_endpoint="
                + FormatUdpEndpoint(first->endpoint) + ". err="
                + std::to_string(GetSocketError()));
        }
        if (!SendMessage(sock, second->endpoint, "NAT_PEER_INFO",
                         NatPeerInfoFields(*second, *first))) {
            Log(LogLevel::Warn, "Failed to send NAT_PEER_INFO to peer="
                + second->node + " control_endpoint="
                + FormatUdpEndpoint(second->endpoint) + ". err="
                + std::to_string(GetSocketError()));
        }
    }

    void HandleNatInfo(Room& room, Room::iterator current,
                       const UdpEndpoint& source,
                       const std::vector<std::string>& fields,
                       std::chrono::steady_clock::time_point now) {
        uint64_t attemptId = 0;
        uint16_t mappedAPort = 0;
        uint16_t mappedBPort = 0;
        UdpEndpoint mappedA{};
        UdpEndpoint mappedB{};
        Room::iterator target;
        if (!ParseAttemptId(fields[4], &attemptId)
            || fields[5] != kNatPunchProtocolVersion
            || !IsNatMappingBehavior(fields[6])
            || !ParsePort(fields[8], &mappedAPort)
            || !ParsePort(fields[10], &mappedBPort)
            || !ParseUdpEndpoint(fields[7], mappedAPort, &mappedA)
            || !ParseUdpEndpoint(fields[9], mappedBPort, &mappedB)
            || !IsSafeControlField(fields[11])
            || !ValidateNatSession(room, current, fields[2], fields[3],
                                   attemptId, &target)
            || !SameUdpEndpoint(source, current->second.endpoint)) {
            SendMessage(sock, source, "ERROR", {"invalid-nat-session"});
            return;
        }

        current->second.seen = now;
        const bool sameReport = current->second.natPunch.reported
            && current->second.natPunch.behavior == fields[6]
            && SameUdpEndpoint(current->second.natPunch.mappedA, mappedA)
            && SameUdpEndpoint(current->second.natPunch.mappedB, mappedB)
            && current->second.natPunch.localCandidates == fields[11];
        current->second.natPunch.reported = true;
        if (!sameReport) current->second.natPunch.armed = false;
        current->second.natPunch.behavior = fields[6];
        current->second.natPunch.mappedA = mappedA;
        current->second.natPunch.mappedB = mappedB;
        current->second.natPunch.localCandidates = fields[11];
        if (!sameReport) target->second.natPunch.armed = false;

        if (!target->second.natPunch.reported) {
            SendMessage(sock, source, "NAT_WAIT",
                        {fields[3], fields[4]});
            return;
        }

        Log(LogLevel::Info, "NAT info ready room=" + fields[0]
            + " session=" + fields[3] + " attempt_id=" + fields[4]
            + " peer=" + current->first + " control_endpoint="
            + FormatUdpEndpoint(current->second.endpoint)
            + " target=" + target->first + " control_endpoint="
            + FormatUdpEndpoint(target->second.endpoint));
        SendNatPeerInfo(&current->second, &target->second);
    }

    void HandleNatArmed(Room& room, Room::iterator current,
                        const UdpEndpoint& source,
                        const std::vector<std::string>& fields,
                        std::chrono::steady_clock::time_point now) {
        uint64_t attemptId = 0;
        Room::iterator target;
        if (!ParseAttemptId(fields[4], &attemptId)
            || !ValidateNatSession(room, current, fields[2], fields[3],
                                   attemptId, &target)
            || !current->second.natPunch.reported
            || !target->second.natPunch.reported
            || !SameUdpEndpoint(source, current->second.endpoint)) {
            SendMessage(sock, source, "ERROR", {"invalid-nat-session"});
            return;
        }

        current->second.seen = now;
        current->second.natPunch.armed = true;
        if (!target->second.natPunch.armed) {
            SendMessage(sock, source, "NAT_ARMED_ACK",
                        {fields[3], fields[4]});
            return;
        }

        const std::vector<std::string> startFields{fields[3], fields[4]};
        if (!SendMessage(sock, current->second.endpoint,
                         "NAT_START", startFields)) {
            Log(LogLevel::Warn, "Failed to send NAT_START to peer="
                + current->first + " control_endpoint="
                + FormatUdpEndpoint(current->second.endpoint) + ". err="
                + std::to_string(GetSocketError()));
        }
        if (!SendMessage(sock, target->second.endpoint,
                         "NAT_START", startFields)) {
            Log(LogLevel::Warn, "Failed to send NAT_START to peer="
                + target->first + " control_endpoint="
                + FormatUdpEndpoint(target->second.endpoint) + ". err="
                + std::to_string(GetSocketError()));
        }
        Log(LogLevel::Info, "NAT punch start room=" + fields[0]
            + " session=" + fields[3] + " attempt_id=" + fields[4]);
    }

    void HandleNatRetry(Room& room, Room::iterator current,
                        const UdpEndpoint& source,
                        const std::vector<std::string>& fields,
                        std::chrono::steady_clock::time_point now) {
        uint64_t attemptId = 0;
        Room::iterator target;
        if (!ParseAttemptId(fields[4], &attemptId)
            || !IsSafeControlField(fields[3])
            || !ValidateNatPair(room, current, fields[2], fields[3], &target)
            || !SameUdpEndpoint(source, current->second.endpoint)) {
            SendMessage(sock, source, "ERROR", {"invalid-nat-session"});
            return;
        }

        current->second.seen = now;
        if (attemptId < current->second.natAttemptId) {
            SendNatAttempt(current->second);
            return;
        }
        if (attemptId != current->second.natAttemptId
            || target->second.natAttemptId != attemptId) {
            SendMessage(sock, source, "ERROR", {"invalid-nat-session"});
            return;
        }

        current->second.natRetryRequested = true;
        if (!target->second.natRetryRequested) {
            SendMessage(sock, source, "NAT_RETRY_WAIT",
                        {fields[3], fields[4]});
            return;
        }

        std::string randomError;
        const std::string punchToken = SecureRandomHex(32, &randomError);
        if (punchToken.empty()) {
            Log(LogLevel::Error, "Cannot create NAT retry token: " + randomError);
            SendMessage(sock, current->second.endpoint, "ERROR",
                        {"server-random-failed"});
            SendMessage(sock, target->second.endpoint, "ERROR",
                        {"server-random-failed"});
            return;
        }
        uint64_t nextAttemptId = nextNatAttemptId++;
        if (nextAttemptId == 0) nextAttemptId = nextNatAttemptId++;
        current->second.natAttemptId = nextAttemptId;
        current->second.natPunchToken = punchToken;
        current->second.natRetryRequested = false;
        current->second.natPunch = {};
        target->second.natAttemptId = nextAttemptId;
        target->second.natPunchToken = punchToken;
        target->second.natRetryRequested = false;
        target->second.natPunch = {};

        SendNatAttempt(current->second);
        SendNatAttempt(target->second);
        Log(LogLevel::Info, "NAT retry ready room=" + fields[0]
            + " session=" + fields[3]
            + " previous_attempt_id=" + fields[4]
            + " attempt_id=" + std::to_string(nextAttemptId));
    }

    void HandleConnect(Room& room, Room::iterator current,
                       const UdpEndpoint& source,
                       const std::vector<std::string>& fields) {
        const auto target = room.find(fields[2]);
        if (target == room.end() || target == current) {
            Log(LogLevel::Debug, "Target unavailable: room=" + fields[0]
                + " peer=" + current->first + " target=" + fields[2]);
            SendMessage(sock, source, "ERROR", {"peer-not-found"});
            return;
        }
        if ((!current->second.pairedWith.empty()
                && current->second.pairedWith != target->first)
            || (!target->second.pairedWith.empty()
                && target->second.pairedWith != current->first)) {
            Log(LogLevel::Warn, "Target busy: room=" + fields[0]
                + " peer=" + current->first + " target=" + target->first);
            SendMessage(sock, source, "ERROR", {"peer-busy"});
            return;
        }

        std::vector<TraversalMode> negotiatedModes;
        std::string initiator;
        const bool alreadyPaired =
            current->second.pairedWith == target->first
            && target->second.pairedWith == current->first
            && !current->second.negotiatedModes.empty()
            && current->second.negotiatedModes
                == target->second.negotiatedModes;
        if (alreadyPaired) {
            negotiatedModes = current->second.negotiatedModes;
            initiator = current->second.pairInitiator;
        } else {
            negotiatedModes = IntersectTraversalModes(
                current->second.capabilities, target->second.capabilities);
            if (negotiatedModes.empty()) {
                const std::string targetCapabilities =
                    SerializeTraversalModeSequence(target->second.capabilities);
                Log(LogLevel::Warn, "No common traversal mode room=" + fields[0]
                    + " peer=" + current->first
                    + " capabilities="
                    + SerializeTraversalModeSequence(
                        current->second.capabilities)
                    + " target=" + target->first
                    + " capabilities=" + targetCapabilities);
                SendMessage(sock, source, "ERROR",
                    {"no-common-traversal-mode", targetCapabilities});
                return;
            }
            std::string randomError;
            const std::string sessionId = SecureRandomHex(16, &randomError);
            const std::string punchToken = SecureRandomHex(32, &randomError);
            if (sessionId.empty() || punchToken.empty()) {
                Log(LogLevel::Error, "Cannot create NAT session: " + randomError);
                SendMessage(sock, source, "ERROR", {"server-random-failed"});
                return;
            }
            uint64_t attemptId = nextNatAttemptId++;
            if (attemptId == 0) attemptId = nextNatAttemptId++;
            initiator = current->first;
            current->second.pairedWith = target->first;
            current->second.negotiatedModes = negotiatedModes;
            current->second.pairInitiator = initiator;
            target->second.pairedWith = current->first;
            target->second.negotiatedModes = negotiatedModes;
            target->second.pairInitiator = initiator;
            current->second.natSessionId = sessionId;
            current->second.natPunchToken = punchToken;
            current->second.natAttemptId = attemptId;
            current->second.natRetryRequested = false;
            current->second.natPunch = {};
            target->second.natSessionId = sessionId;
            target->second.natPunchToken = punchToken;
            target->second.natAttemptId = attemptId;
            target->second.natRetryRequested = false;
            target->second.natPunch = {};
        }

        const std::string negotiated =
            SerializeTraversalModeSequence(negotiatedModes);
        Log(LogLevel::Info, "Paired room=" + fields[0]
            + " initiator=" + initiator
            + " peer=" + current->first
            + " endpoint=" + FormatUdpEndpoint(source)
            + " capabilities="
            + SerializeTraversalModeSequence(current->second.capabilities)
            + " target=" + target->first
            + " endpoint=" + FormatUdpEndpoint(target->second.endpoint)
            + " capabilities="
            + SerializeTraversalModeSequence(target->second.capabilities)
            + " traversal_modes=" + negotiated);
        const auto currentAddress = IpAndPort(source);
        const auto targetAddress = IpAndPort(target->second.endpoint);
        SendMessage(sock, target->second.endpoint, "PEER",
                    {currentAddress.first, currentAddress.second, current->first,
                     SerializeTraversalModeSequence(
                         current->second.capabilities),
                         negotiated,
                         target->second.natSessionId,
                         std::to_string(target->second.natAttemptId),
                         target->first == initiator ? "initiator" : "responder",
                         kNatPunchProtocolVersion,
                         target->second.natPunchToken});
        SendMessage(sock, source, "PEER",
                    {targetAddress.first, targetAddress.second, target->first,
                     SerializeTraversalModeSequence(
                         target->second.capabilities),
                         negotiated,
                         current->second.natSessionId,
                         std::to_string(current->second.natAttemptId),
                         current->first == initiator ? "initiator" : "responder",
                         kNatPunchProtocolVersion,
                         current->second.natPunchToken});
    }

    void HandleIpv6Join(Room& room, Room::iterator current,
                        const UdpEndpoint& source,
                        const std::vector<std::string>& fields) {
        in6_addr ipv6{};
        uint16_t port = 0;
        if (!ParseIpv6(fields[3], &ipv6) || !IsIpv6Gua(ipv6)
            || !ParsePort(fields[4], &port)
            || (fields[5] != "0" && fields[5] != "1")) {
            SendMessage(sock, source, "ERROR", {"invalid-ipv6-endpoint"});
            return;
        }

        const auto target = room.find(fields[2]);
        if ((!current->second.pairedWith.empty()
                && current->second.pairedWith != fields[2])
            || (target != room.end() && target != current
                && !target->second.pairedWith.empty()
                && target->second.pairedWith != current->first)) {
            SendMessage(sock, source, "ERROR", {"peer-busy"});
            return;
        }

        current->second.endpoint = source;
        current->second.pairedWith = fields[2];
        current->second.ipv6Address = fields[3];
        current->second.ipv6Port = port;
        current->second.ipv6AcceptInbound = fields[5] == "1";
        current->second.ipv6Joined = true;

        if (target == room.end() || target == current
            || !target->second.ipv6Joined
            || target->second.pairedWith != current->first) {
            SendMessage(sock, source, "V6_WAIT");
            return;
        }
        if (!current->second.ipv6AcceptInbound
            && !target->second.ipv6AcceptInbound) {
            SendMessage(sock, source, "ERROR", {"ipv6-no-inbound-peer"});
            SendMessage(sock, target->second.endpoint, "ERROR",
                        {"ipv6-no-inbound-peer"});
            return;
        }

        const bool currentListens = current->second.ipv6AcceptInbound
            && (!target->second.ipv6AcceptInbound
                || current->first < target->first);
        const std::string currentRole = currentListens ? "listen" : "connect";
        const std::string targetRole = currentListens ? "connect" : "listen";
        Log(LogLevel::Info, "IPv6 fallback ready room=" + fields[0]
            + " peer=" + current->first + " endpoint=["
            + current->second.ipv6Address + "]:"
            + std::to_string(current->second.ipv6Port) + " role=" + currentRole
            + " target=" + target->first + " endpoint=["
            + target->second.ipv6Address + "]:"
            + std::to_string(target->second.ipv6Port) + " role=" + targetRole);
        SendMessage(sock, target->second.endpoint, "V6_PEER",
                    {current->second.ipv6Address,
                     std::to_string(current->second.ipv6Port), current->first,
                     targetRole});
        SendMessage(sock, source, "V6_PEER",
                    {target->second.ipv6Address,
                     std::to_string(target->second.ipv6Port), target->first,
                     currentRole});
    }

    void HandleIpv4RelayJoin(Room& room, Room::iterator current,
                             const UdpEndpoint& source,
                             const std::vector<std::string>& fields,
                             std::chrono::steady_clock::time_point now) {
        const auto target = room.find(fields[2]);
        if (target == current) {
            SendMessage(sock, source, "ERROR", {"peer-not-found"});
            return;
        }
        if (target == room.end()) {
            current->second.endpoint = source;
            current->second.seen = now;
            current->second.pairedWith = fields[2];
            SendMessage(sock, source, "RELAY_WAIT");
            return;
        }
        if ((!current->second.pairedWith.empty()
                && current->second.pairedWith != target->first)
            || (!target->second.pairedWith.empty()
                && target->second.pairedWith != current->first)) {
            SendMessage(sock, source, "ERROR", {"peer-busy"});
            return;
        }
        current->second.endpoint = source;
        current->second.seen = now;
        current->second.pairedWith = target->first;
        target->second.pairedWith = current->first;
        relayApp.HandleJoin(source, fields[0], current->first, target->first, now);
    }

    void HandleTunIp(const UdpEndpoint& source,
                     const std::vector<std::string>& fields,
                     std::chrono::steady_clock::time_point now) {
        const std::string& roomId = fields[0];
        const std::string& nodeId = fields[1];
        const std::string& tunIp = fields[2];
        const std::string& token = fields[3];
        in_addr parsedIp{};
        if (!Authorized(roomId, nodeId, "", token)) {
            Log(LogLevel::Warn, "Rejected TUN_IP from " + FormatUdpEndpoint(source));
            SendMessage(sock, source, "ERROR", {"unauthorized"});
            return;
        }
        if (!ParseIpv4(tunIp, &parsedIp)) {
            Log(LogLevel::Warn, "Rejected invalid TUN IP from "
                + FormatUdpEndpoint(source));
            SendMessage(sock, source, "ERROR", {"invalid-tun-ip"});
            return;
        }

        const auto room = rooms.find(roomId);
        if (room == rooms.end()) return;
        const auto current = room->second.find(nodeId);
        if (current == room->second.end()) return;

        current->second.seen = now;
        current->second.endpoint = source;
        if (current->second.tunIp == tunIp) return;
        current->second.tunIp = tunIp;
        Log(LogLevel::Info, "TUN IP reported peer=" + nodeId + " room=" + roomId
            + " tun_ip=" + tunIp + " endpoint=" + FormatUdpEndpoint(source));
    }

    void HandleSession(const UdpEndpoint& source, const std::string& type,
                       const std::vector<std::string>& fields,
                       std::chrono::steady_clock::time_point now) {
        size_t expectedFields = 0;
        if ((type == "REG" && fields.size() == 4)
            || (type == "CONNECT" && fields.size() == 5)) {
            Log(LogLevel::Warn, "Rejected legacy rendezvous handshake type="
                + type + " from " + FormatUdpEndpoint(source));
            SendProtocolVersionMismatch(sock, source, "legacy");
            return;
        }
        if (type == "REG") expectedFields = 5;
        else if (type == "CONNECT") expectedFields = 6;
        else if (type == "UNREG") expectedFields = 3;
        else if (type == "NAT_INFO") expectedFields = 13;
        else if (type == "NAT_ARMED") expectedFields = 6;
        else if (type == "NAT_RETRY") expectedFields = 6;
        else if (type == "V6_JOIN") expectedFields = 7;
        else if (type == "RELAY_JOIN") expectedFields = 4;
        else if (type == "TUN_IP") expectedFields = 4;

        const std::string loggedType =
            IsSafeControlField(type) ? type : "<invalid>";
        if (expectedFields == 0) {
            Log(LogLevel::Debug, "Ignored rendezvous session message type="
                + loggedType + " from " + FormatUdpEndpoint(source)
                + ": unknown message type, fields="
                + std::to_string(fields.size()));
            return;
        }
        if (fields.size() != expectedFields) {
            Log(LogLevel::Debug, "Ignored rendezvous session message type="
                + loggedType + " from " + FormatUdpEndpoint(source)
                + ": field count mismatch, expected="
                + std::to_string(expectedFields) + ", actual="
                + std::to_string(fields.size()));
            return;
        }

        const bool isRegister = type == "REG";
        const bool isConnect = type == "CONNECT";
        const bool isUnregister = type == "UNREG";
        const bool isNatInfo = type == "NAT_INFO";
        const bool isNatArmed = type == "NAT_ARMED";
        const bool isNatRetry = type == "NAT_RETRY";
        const bool isIpv6Join = type == "V6_JOIN";
        const bool isIpv4RelayJoin = type == "RELAY_JOIN";
        const bool isTunIp = type == "TUN_IP";

        if ((isRegister && fields[3] != kNatPunchProtocolVersion)
            || (isConnect && fields[4] != kNatPunchProtocolVersion)
            || (isNatInfo && fields[5] != kNatPunchProtocolVersion)) {
            const std::string& received = fields[
                isRegister ? 3 : isConnect ? 4 : 5];
            Log(LogLevel::Warn, "Rejected NAT punch protocol version="
                + received + " type=" + type + " from "
                + FormatUdpEndpoint(source));
            SendProtocolVersionMismatch(sock, source, received);
            return;
        }

        if (isTunIp) {
            HandleTunIp(source, fields, now);
            return;
        }

        const std::string& roomId = fields[0];
        const std::string& nodeId = fields[1];
        const std::string targetId = (isConnect || isNatInfo
                                      || isNatArmed || isNatRetry || isIpv6Join
                                      || isIpv4RelayJoin)
            ? fields[2] : "";
        size_t tokenIndex = 2;
        if (isRegister) tokenIndex = 4;
        if (isConnect) tokenIndex = 5;
        if (isIpv4RelayJoin) tokenIndex = 3;
        if (isNatArmed) tokenIndex = 5;
        if (isNatRetry) tokenIndex = 5;
        if (isIpv6Join) tokenIndex = 6;
        if (isNatInfo) tokenIndex = 12;
        const std::string& token = fields[tokenIndex];
        if (!Authorized(roomId, nodeId, targetId, token)) {
            Log(LogLevel::Warn, "Rejected " + type + " from " + FormatUdpEndpoint(source));
            SendMessage(sock, source, "ERROR", {"unauthorized"});
            return;
        }

        std::vector<TraversalMode> capabilities;
        if (isRegister || isConnect) {
            const std::string& capabilityText =
                fields[isRegister ? 2 : 3];
            std::string capabilityError;
            if (!IsSafeControlField(capabilityText)
                || !ParseTraversalModeSequence(
                    capabilityText, &capabilities, &capabilityError)) {
                Log(LogLevel::Warn, "Rejected invalid traversal capabilities from "
                    + FormatUdpEndpoint(source));
                SendMessage(sock, source, "ERROR",
                            {"invalid-traversal-capabilities"});
                return;
            }
        }

        auto& room = rooms[roomId];
        const size_t expired = RemoveExpired(&room, now, config.clientTimeoutSeconds);
        if (expired > 0) {
            Log(LogLevel::Info, "Expired " + std::to_string(expired)
                + " client(s) from room=" + roomId);
        }
        if (isUnregister) {
            const auto existing = room.find(nodeId);
            if (existing != room.end() && SameUdpEndpoint(existing->second.endpoint, source)) {
                relayApp.RemovePeer(roomId, nodeId);
                room.erase(existing);
                for (auto& entry : room) {
                    if (entry.second.pairedWith == nodeId) {
                        ResetPairing(&entry.second);
                    }
                }
                Log(LogLevel::Info, "Unregistered peer=" + nodeId + " room=" + roomId
                    + " endpoint=" + FormatUdpEndpoint(source));
            }
            return;
        }
        if (room.find(nodeId) == room.end()
            && room.size() >= config.maxClientsPerRoom) {
            Log(LogLevel::Warn, "Room full: room=" + roomId + " peer=" + nodeId);
            SendMessage(sock, source, "ERROR", {"room-full"});
            return;
        }

        auto current = room.find(nodeId);
        if ((isNatInfo || isNatArmed || isNatRetry)
            && current == room.end()) {
            SendMessage(sock, source, "ERROR", {"invalid-nat-session"});
            return;
        }
        if (current == room.end()) {
            Client client;
            client.node = nodeId;
            client.endpoint = source;
            client.seen = now;
            current = room.emplace(nodeId, std::move(client)).first;
            Log(LogLevel::Info, "Registered peer=" + nodeId + " room=" + roomId
                + " endpoint=" + FormatUdpEndpoint(source) + " tun_ip=N/A");
        } else {
            if (!isNatInfo && !isNatArmed && !isNatRetry) {
                current->second.endpoint = source;
            }
            current->second.seen = now;
        }
        if (isRegister || isConnect) {
            if (current->second.capabilities != capabilities
                && !current->second.pairedWith.empty()) {
                const auto peer = room.find(current->second.pairedWith);
                if (peer != room.end()
                    && peer->second.pairedWith == current->first) {
                    ResetPairing(&peer->second);
                }
                ResetPairing(&current->second);
            }
            current->second.capabilities = std::move(capabilities);
        }
        if (isRegister) {
            current->second.ipv6Joined = false;
            SendMessage(sock, source, "REGISTERED",
                        {kNatPunchProtocolVersion});
        } else if (isNatInfo) {
            HandleNatInfo(room, current, source, fields, now);
        } else if (isNatArmed) {
            HandleNatArmed(room, current, source, fields, now);
        } else if (isNatRetry) {
            HandleNatRetry(room, current, source, fields, now);
        } else if (isIpv6Join) {
            HandleIpv6Join(room, current, source, fields);
        } else if (isIpv4RelayJoin) {
            HandleIpv4RelayJoin(room, current, source, fields, now);
        } else {
            HandleConnect(room, current, source, fields);
        }
    }
};

RendezvousRegistry::RendezvousRegistry(socket_t sock, const RendezvousConfig& config)
    : impl_(std::make_unique<Impl>(sock, config)) {}

RendezvousRegistry::~RendezvousRegistry() = default;

void RendezvousRegistry::Handle(const UdpEndpoint& source, const std::string& type,
                                const std::vector<std::string>& fields,
                                std::chrono::steady_clock::time_point now) {
    if (type == "LIST") {
        impl_->HandleList(source, fields, now);
    } else {
        impl_->HandleSession(source, type, fields, now);
    }
}

RendezvousRelaySnapshot RendezvousRegistry::RelaySnapshot() {
    const Ipv4RelayAppSnapshot relay = impl_->relayApp.Snapshot();
    RendezvousRelaySnapshot snapshot;
    snapshot.activeSessions = relay.activeSessions;
    snapshot.receivedDatagrams = relay.receivedDatagrams;
    snapshot.forwardedDatagrams = relay.forwardedDatagrams;
    snapshot.forwardedBytes = relay.forwardedBytes;
    snapshot.sessions.reserve(relay.sessions.size());
    for (const auto& relaySession : relay.sessions) {
        RendezvousRelaySnapshot::Session session;
        session.roomId = relaySession.roomId;
        session.port = relaySession.port;
        session.ready = relaySession.ready;
        for (int side = 0; side < 2; ++side) {
            session.peers[side].nodeId = relaySession.peers[side].nodeId;
            session.peers[side].endpoint = relaySession.peers[side].endpoint;
            session.peers[side].idleSeconds =
                relaySession.peers[side].idleSeconds;
            session.peers[side].connected =
                relaySession.peers[side].connected;
        }
        snapshot.sessions.push_back(std::move(session));
    }
    return snapshot;
}

std::vector<RendezvousRoomSnapshot> RendezvousRegistry::Snapshot(
    std::chrono::steady_clock::time_point now) {
    std::vector<RendezvousRoomSnapshot> snapshot;
    for (auto room = impl_->rooms.begin(); room != impl_->rooms.end();) {
        const size_t expired = RemoveExpired(
            &room->second, now, impl_->config.clientTimeoutSeconds);
        if (expired > 0) {
            Log(LogLevel::Info, "Expired " + std::to_string(expired)
                + " client(s) from room=" + room->first);
        }
        if (room->second.empty()) {
            room = impl_->rooms.erase(room);
            continue;
        }

        RendezvousRoomSnapshot roomSnapshot;
        roomSnapshot.roomId = room->first;
        roomSnapshot.clients.reserve(room->second.size());
        for (const auto& entry : room->second) {
            const Client& client = entry.second;
            const auto idle = std::chrono::duration_cast<std::chrono::seconds>(
                now - client.seen).count();
            roomSnapshot.clients.push_back({
                client.node,
                FormatUdpEndpoint(client.endpoint),
                client.tunIp.empty() ? "N/A" : client.tunIp,
                client.pairedWith,
                static_cast<uint64_t>((std::max)(int64_t{0}, idle)),
                client.natPunch.reported,
                client.natPunch.armed,
            });
        }
        std::sort(roomSnapshot.clients.begin(), roomSnapshot.clients.end(),
                  [](const auto& left, const auto& right) {
                      return left.nodeId < right.nodeId;
                  });
        snapshot.push_back(std::move(roomSnapshot));
        ++room;
    }
    std::sort(snapshot.begin(), snapshot.end(), [](const auto& left, const auto& right) {
        return left.roomId < right.roomId;
    });
    return snapshot;
}
