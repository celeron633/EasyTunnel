#include "registry.h"

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>

#include "../log.h"
#include "../nat_protocol.h"

namespace {
struct Client {
    std::string node;
    UdpEndpoint endpoint;
    std::chrono::steady_clock::time_point seen;
    std::string pairedWith;
    uint32_t nat4Round = 0;
    bool nat4Joined = false;
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
}  // namespace

struct RendezvousRegistry::Impl {
    Impl(socket_t socket, const RendezvousConfig& settings)
        : sock(socket), config(settings) {}

    socket_t sock;
    RendezvousConfig config;
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

    void HandleSession(const UdpEndpoint& source, const std::string& type,
                       const std::vector<std::string>& fields,
                       std::chrono::steady_clock::time_point now) {
        const bool isRegister = type == "REG" && fields.size() == 3;
        const bool isConnect = type == "CONNECT" && fields.size() == 4;
        const bool isUnregister = type == "UNREG" && fields.size() == 3;
        const bool isNat4Join = type == "NAT4_JOIN" && fields.size() == 5;
        if (!isRegister && !isConnect && !isUnregister && !isNat4Join) return;

        const std::string& roomId = fields[0];
        const std::string& nodeId = fields[1];
        const std::string targetId = (isConnect || isNat4Join) ? fields[2] : "";
        const std::string& token = fields[isNat4Join ? 4 : (isConnect ? 3 : 2)];
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
                room.erase(existing);
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
                Client{nodeId, source, now, "", 0, false}).first;
            Log(LogLevel::Info, "Registered peer=" + nodeId + " room=" + roomId
                + " endpoint=" + FormatUdpEndpoint(source));
        } else {
            current->second.endpoint = source;
            current->second.seen = now;
        }
        if (isRegister) {
            current->second.nat4Joined = false;
            SendMessage(sock, source, "REGISTERED");
        } else if (isNat4Join) {
            HandleNat4Join(room, current, source, fields);
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
