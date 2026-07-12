#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "nat_protocol.h"
#include "rendezvous_config.h"
#include "util.h"

namespace {
std::atomic<bool> running{true};
struct Client {
    std::string node;
    UdpEndpoint endpoint;
    std::chrono::steady_clock::time_point seen;
    std::string pairedWith;
};
using Room = std::unordered_map<std::string, Client>;

void Stop(int) { running.store(false); }

bool Send(socket_t sock, const UdpEndpoint& endpoint, const uint8_t* data, size_t len) {
    return sendto(sock, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
        reinterpret_cast<const sockaddr*>(&endpoint.addr), endpoint.addr_len) == static_cast<int>(len);
}

std::pair<std::string, std::string> IpAndPort(const UdpEndpoint& endpoint) {
    const auto* address = reinterpret_cast<const sockaddr_in*>(&endpoint.addr);
    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &address->sin_addr, ip, sizeof(ip));
    return {ip, std::to_string(ntohs(address->sin_port))};
}

void SendMessage(socket_t sock, const UdpEndpoint& endpoint, const std::string& type,
                 const std::vector<std::string>& fields = {}) {
    const std::string message = MakeControlMessage(type, fields);
    Send(sock, endpoint, reinterpret_cast<const uint8_t*>(message.data()), message.size());
}

size_t RemoveExpired(Room* room, std::chrono::steady_clock::time_point now,
                     uint16_t timeoutSeconds) {
    size_t removed = 0;
    for (auto it = room->begin(); it != room->end();) {
        if (now - it->second.seen > std::chrono::seconds(timeoutSeconds)) {
            it = room->erase(it);
            ++removed;
        }
        else ++it;
    }
    return removed;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "Usage: EasyTunnel_rendezvous [config.json]\n";
        return 2;
    }
    const std::string configPath = argc > 1 ? argv[1] : "EasyTunnel_rendezvous.json";
    RendezvousConfig config;
    bool configCreated = false;
    std::string configError;
    if (!LoadOrCreateRendezvousConfig(configPath, &config, &configCreated, &configError)) {
        std::cerr << configError << '\n';
        return 2;
    }
    SetLogFilePath(config.logFile);
    SetLogLevel(config.logLevel);
    if (configCreated) {
        Log(LogLevel::Info, "Created default configuration: " + configPath);
    }
    Log(LogLevel::Info, "Loaded configuration: " + configPath
        + ", log_level=" + LevelToString(config.logLevel));
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        Log(LogLevel::Error, "WSAStartup failed");
        return 1;
    }
#endif
    std::signal(SIGINT, Stop);
    std::signal(SIGTERM, Stop);
    UdpEndpoint local{};
    if (!ParseUdpEndpoint(config.bindAddress, config.port, &local) || local.family != AF_INET) {
        Log(LogLevel::Error, "bind_address must be an IPv4 address");
        return 2;
    }
    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSocket
        || bind(sock, reinterpret_cast<const sockaddr*>(&local.addr), local.addr_len) != 0) {
        Log(LogLevel::Error, "Cannot bind " + config.bindAddress + ":"
            + std::to_string(config.port) + ", error=" + std::to_string(GetSocketError()));
        return 1;
    }
    SetSocketRecvTimeoutMs(sock, 1000);
    Log(LogLevel::Info, "EasyTunnel rendezvous listening on "
        + config.bindAddress + ":" + std::to_string(config.port) + " (IPv4/UDP)");
    Log(LogLevel::Info, "Room capacity=" + std::to_string(config.maxClientsPerRoom)
        + ", client timeout=" + std::to_string(config.clientTimeoutSeconds) + "s");

    std::unordered_map<std::string, Room> rooms;
    std::vector<uint8_t> buffer(2048);
    while (running.load()) {
        sockaddr_storage sourceAddress{};
        socket_len_t sourceLen = static_cast<socket_len_t>(sizeof(sourceAddress));
        const int n = recvfrom(sock, reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&sourceAddress), &sourceLen);
        if (n < 0) {
            if (IsRecvTimeout(GetSocketError())) continue;
            Log(LogLevel::Error, "recvfrom failed. error=" + std::to_string(GetSocketError()));
            continue;
        }
        UdpEndpoint source{};
        source.addr = sourceAddress;
        source.addr_len = sourceLen;
        source.family = sourceAddress.ss_family;
        std::string type;
        std::vector<std::string> fields;
        if (!ParseControlMessage(buffer.data(), n, &type, &fields)) continue;
        const auto now = std::chrono::steady_clock::now();

        if (type == "LIST" && fields.size() == 2) {
            if (!IsSafeControlField(fields[0])
                || (!fields[1].empty() && !IsSafeControlField(fields[1]))
                || (!config.authToken.empty() && fields[1] != config.authToken)) {
                Log(LogLevel::Warn, "Rejected LIST from " + FormatUdpEndpoint(source));
                SendMessage(sock, source, "ERROR", {"unauthorized"});
                continue;
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
            continue;
        }

        const bool isRegister = type == "REG" && fields.size() == 3;
        const bool isConnect = type == "CONNECT" && fields.size() == 4;
        const bool isUnregister = type == "UNREG" && fields.size() == 3;
        if (!isRegister && !isConnect && !isUnregister) continue;
        const std::string& roomId = fields[0];
        const std::string& nodeId = fields[1];
        const std::string& token = fields[isConnect ? 3 : 2];
        if (!IsSafeControlField(roomId) || !IsSafeControlField(nodeId)
            || (!token.empty() && !IsSafeControlField(token))
            || (isConnect && !IsSafeControlField(fields[2]))
            || (!config.authToken.empty() && token != config.authToken)) {
            Log(LogLevel::Warn, "Rejected " + type + " from " + FormatUdpEndpoint(source));
            SendMessage(sock, source, "ERROR", {"unauthorized"});
            continue;
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
            continue;
        }
        if (room.find(nodeId) == room.end()
            && room.size() >= config.maxClientsPerRoom) {
            Log(LogLevel::Warn, "Room full: room=" + roomId + " peer=" + nodeId);
            SendMessage(sock, source, "ERROR", {"room-full"});
            continue;
        }
        auto current = room.find(nodeId);
        if (current == room.end()) {
            current = room.emplace(nodeId, Client{nodeId, source, now, ""}).first;
            Log(LogLevel::Info, "Registered peer=" + nodeId + " room=" + roomId
                + " endpoint=" + FormatUdpEndpoint(source));
        } else {
            current->second.endpoint = source;
            current->second.seen = now;
        }
        if (isRegister) {
            SendMessage(sock, source, "REGISTERED");
            continue;
        }

        const auto target = room.find(fields[2]);
        if (target == room.end() || target->first == nodeId) {
            Log(LogLevel::Debug, "Target unavailable: room=" + roomId
                + " peer=" + nodeId + " target=" + fields[2]);
            SendMessage(sock, source, "ERROR", {"peer-not-found"});
            continue;
        }
        if ((!current->second.pairedWith.empty()
                && current->second.pairedWith != target->first)
            || (!target->second.pairedWith.empty()
                && target->second.pairedWith != nodeId)) {
            Log(LogLevel::Warn, "Target busy: room=" + roomId
                + " peer=" + nodeId + " target=" + target->first);
            SendMessage(sock, source, "ERROR", {"peer-busy"});
            continue;
        }
        current->second.pairedWith = target->first;
        target->second.pairedWith = nodeId;
        Log(LogLevel::Info, "Paired room=" + roomId + " peer=" + nodeId
            + " endpoint=" + FormatUdpEndpoint(source) + " target=" + target->first
            + " endpoint=" + FormatUdpEndpoint(target->second.endpoint));
        const auto currentAddress = IpAndPort(source);
        const auto targetAddress = IpAndPort(target->second.endpoint);
        SendMessage(sock, target->second.endpoint, "PEER",
                    {currentAddress.first, currentAddress.second, nodeId});
        SendMessage(sock, source, "PEER",
                    {targetAddress.first, targetAddress.second, target->first});
    }
    CloseSocket(sock);
    Log(LogLevel::Info, "EasyTunnel rendezvous stopped");
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
