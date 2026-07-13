#include "server.h"

#include <chrono>
#include <string>
#include <vector>

#include "../log.h"
#include "../nat_protocol.h"
#include "../util.h"
#include "registry.h"

RendezvousServer::RendezvousServer(const RendezvousConfig& config,
                                   const std::atomic<bool>& running)
    : config_(config), running_(running) {}

int RendezvousServer::Run() {
    UdpEndpoint local{};
    if (!ParseUdpEndpoint(config_.bindAddress, config_.port, &local)
        || local.family != AF_INET) {
        Log(LogLevel::Error, "bind_address must be an IPv4 address");
        return 2;
    }

    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSocket
        || bind(sock, reinterpret_cast<const sockaddr*>(&local.addr), local.addr_len) != 0) {
        Log(LogLevel::Error, "Cannot bind " + config_.bindAddress + ":"
            + std::to_string(config_.port) + ", error="
            + std::to_string(GetSocketError()));
        CloseSocket(sock);
        return 1;
    }
    SetSocketRecvTimeoutMs(sock, 1000);
    Log(LogLevel::Info, "EasyTunnel rendezvous listening on "
        + config_.bindAddress + ":" + std::to_string(config_.port) + " (IPv4/UDP)");
    Log(LogLevel::Info, "Room capacity=" + std::to_string(config_.maxClientsPerRoom)
        + ", client timeout=" + std::to_string(config_.clientTimeoutSeconds) + "s");

    RendezvousRegistry registry(sock, config_);
    std::vector<uint8_t> buffer(2048);
    while (running_.load()) {
        sockaddr_storage sourceAddress{};
        socket_len_t sourceLen = static_cast<socket_len_t>(sizeof(sourceAddress));
        const int n = recvfrom(sock, reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&sourceAddress), &sourceLen);
        if (n < 0) {
            if (IsRecvTimeout(GetSocketError())) continue;
            Log(LogLevel::Error, "recvfrom failed. error="
                + std::to_string(GetSocketError()));
            continue;
        }

        UdpEndpoint source{};
        source.addr = sourceAddress;
        source.addr_len = sourceLen;
        source.family = sourceAddress.ss_family;
        std::string type;
        std::vector<std::string> fields;
        if (!ParseControlMessage(buffer.data(), static_cast<size_t>(n), &type, &fields)) {
            continue;
        }
        registry.Handle(source, type, fields, std::chrono::steady_clock::now());
    }

    CloseSocket(sock);
    Log(LogLevel::Info, "EasyTunnel rendezvous stopped");
    return 0;
}
