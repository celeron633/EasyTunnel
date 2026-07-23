#include "registry.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>

#include "../log.h"
#include "../nat_protocol.h"
#include "ipv4_relay_app.h"

namespace {
struct Client {
    std::string node;
    UdpEndpoint endpoint;
    std::chrono::steady_clock::time_point seen;
    std::string tunIp;
    std::string pairedWith;
    uint32_t nat4Round = 0;
    bool nat4Joined = false;
    std::string ipv6Address;
    uint16_t ipv6Port = 0;
    bool ipv6AcceptInbound = false;
    bool ipv6Joined = false;
};
using Room = std::unordered_map<std::string, Client>;

bool Send(socket_t sock, const UdpEndpoint& endpoint, const std::string& data) {
    return sendto(sock, data.data(), static_cast<int>(data.size()), 0,
        reinterpret_cast<const sockaddr*>(&endpoint.addr), endpoint.addr_len)
        == static_cast<int>(data.size());
}

void SendMessage(socket_t sock, const UdpEndpoint& endpoint, const std::string& type,
                 const std::vector<std::string>& fields = {}) {
    Send(sock, endpoint, MakeControlMessage(type, fields));
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
            entry.second.pairedWith.clear();
            entry.second.nat4Joined = false;
            entry.second.ipv6Joined = false;
        }
    }
    return removed;
}

bool ParseRound(const std::string& text, uint32_t* round) {
    try {
        size_t consumed = 0;
        const unsigned long parsed = std::stoul(text, &consumed);
        if (consumed != text.size()
            || parsed > (std::numeric_limits<uint32_t>::max)()) return false;
        *round = static_cast<uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
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

bool IsIpv6Gua(const in6_addr& address) {
    return (address.s6_addr[0] & 0xe0) == 0x20;
}
}  // namespace

struct RendezvousRegistry::Impl {
    Impl(socket_t socket, const RendezvousConfig& settings)
        : sock(socket), config(settings), relayApp(socket, settings) {}

    socket_t sock;
    RendezvousConfig config;
    Ipv4RelayApp relayApp;
    std::unordered_map<std::string, Room> rooms;

    bool Authorized(const std::string& roomId, const std::string& nodeId,
                    const std::string& targetId, const std::string& token) const {
        return IsSafeControlField(roomId) && IsSafeControlField(nodeId)
            && (targetId.empty() || IsSafeControlField(targetId))
            && (token.empty() || IsSafeControlField(token))
            && (config.authToken.empty() || token == config.authToken);
    }

    void HandleList(const UdpEndpoint& source, const std::vector<std::string>& fields,
                    std::chrono::steady_clock::time_point now) {
        if (fields.size() != 2 || !IsSafeControlField(fields[0])
            || (!fields[1].empty() && !IsSafeControlField(fields[1]))
            || (!config.authToken.empty() && fields[1] != config.authToken)) {
            Log(LogLevel::Warn, "Rejected LIST from " + FormatUdpEndpoint(source));
            SendMessage(sock, source, "ERROR", {"unauthorized"});
            return;
        }
        std::vector<std::string> clients;
        auto roomIt = rooms.find(fields[0]);
        if (roomIt != rooms.end()) {
            const size_t expired = RemoveExpired(&roomIt->second, now,
                config.clientTimeoutSeconds);
            if (expired > 0) {
                Log(LogLevel::Info, "Expired " + std::to_string(expired)
                    + " client(s) from room=" + fields[0]);
            }
            for (const auto& entry : roomIt->second) {
                if (entry.second.pairedWith.empty()) clients.push_back(entry.first);
            }
        }
        Log(LogLevel::Debug, "LIST room=" + fields[0] + " clients="
            + std::to_string(clients.size()) + " source=" + FormatUdpEndpoint(source));
        SendMessage(sock, source, "CLIENTS", clients);
    }

    void HandleNat4Join(Room& room, Room::iterator current,
                        const UdpEndpoint& source,
                        const std::vector<std::string>& fields) {
        uint32_t round = 0;
        if (!ParseRound(fields[3], &round)) {
            SendMessage(sock, source, "ERROR", {"invalid-nat4-round"});
            return;
        }
        current->second.nat4Round = round;
        current->second.nat4Joined = true;
        current->second.pairedWith = fields[2];

        const auto target = room.find(fields[2]);
        if (target == room.end() || target == current
            || !target->second.nat4Joined || target->second.nat4Round != round
            || (!target->second.pairedWith.empty()
                && target->second.pairedWith != current->first)) {
            SendMessage(sock, source, "NAT4_WAIT", {std::to_string(round)});
            return;
        }

        target->second.pairedWith = current->first;
        const auto currentAddress = IpAndPort(source);
        const auto targetAddress = IpAndPort(target->second.endpoint);
        Log(LogLevel::Info, "NAT4 ready room=" + fields[0]
            + " round=" + std::to_string(round) + " peer=" + current->first
            + " endpoint=" + FormatUdpEndpoint(source) + " target=" + target->first
            + " endpoint=" + FormatUdpEndpoint(target->second.endpoint));
        SendMessage(sock, target->second.endpoint, "NAT4_PEER",
                    {currentAddress.first, currentAddress.second, current->first,
                     std::to_string(round)});
        SendMessage(sock, source, "NAT4_PEER",
                    {targetAddress.first, targetAddress.second, target->first,
                     std::to_string(round)});
    }

    void HandleConnect(Room& room, Room::iterator current,
                       const UdpEndpoint& source,
                       const std::vector<std::string>& fields) {
        current->second.nat4Joined = false;
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
        current->second.pairedWith = target->first;
        target->second.pairedWith = current->first;
        Log(LogLevel::Info, "Paired room=" + fields[0] + " peer=" + current->first
            + " endpoint=" + FormatUdpEndpoint(source) + " target=" + target->first
            + " endpoint=" + FormatUdpEndpoint(target->second.endpoint));
        const auto currentAddress = IpAndPort(source);
        const auto targetAddress = IpAndPort(target->second.endpoint);
        SendMessage(sock, target->second.endpoint, "PEER",
                    {currentAddress.first, currentAddress.second, current->first});
        SendMessage(sock, source, "PEER",
                    {targetAddress.first, targetAddress.second, target->first});
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
        const bool isRegister = type == "REG" && fields.size() == 3;
        const bool isConnect = type == "CONNECT" && fields.size() == 4;
        const bool isUnregister = type == "UNREG" && fields.size() == 3;
        const bool isNat4Join = type == "NAT4_JOIN" && fields.size() == 5;
        const bool isIpv6Join = type == "V6_JOIN" && fields.size() == 7;
        const bool isIpv4RelayJoin = type == "RELAY_JOIN" && fields.size() == 4;
        const bool isTunIp = type == "TUN_IP" && fields.size() == 4;
        if (!isRegister && !isConnect && !isUnregister && !isNat4Join
            && !isIpv6Join && !isIpv4RelayJoin && !isTunIp) return;

        if (isTunIp) {
            HandleTunIp(source, fields, now);
            return;
        }

        const std::string& roomId = fields[0];
        const std::string& nodeId = fields[1];
        const std::string targetId = (isConnect || isNat4Join || isIpv6Join
                                      || isIpv4RelayJoin)
            ? fields[2] : "";
        const std::string& token = fields[isIpv6Join ? 6
            : (isNat4Join ? 4
                : ((isConnect || isIpv4RelayJoin) ? 3 : 2))];
        if (!Authorized(roomId, nodeId, targetId, token)) {
            Log(LogLevel::Warn, "Rejected " + type + " from " + FormatUdpEndpoint(source));
            SendMessage(sock, source, "ERROR", {"unauthorized"});
            return;
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
                        entry.second.pairedWith.clear();
                        entry.second.nat4Joined = false;
                        entry.second.ipv6Joined = false;
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
        if (current == room.end()) {
            current = room.emplace(nodeId,
                Client{nodeId, source, now, "", "", 0, false}).first;
            Log(LogLevel::Info, "Registered peer=" + nodeId + " room=" + roomId
                + " endpoint=" + FormatUdpEndpoint(source) + " tun_ip=N/A");
        } else {
            current->second.endpoint = source;
            current->second.seen = now;
        }
        if (isRegister) {
            current->second.nat4Joined = false;
            current->second.ipv6Joined = false;
            SendMessage(sock, source, "REGISTERED");
        } else if (isNat4Join) {
            HandleNat4Join(room, current, source, fields);
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
                client.nat4Round,
                client.nat4Joined,
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
