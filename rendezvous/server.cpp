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
    : config_(config), running_(running) {
    snapshot_.endpoint = config_.bindAddress + ":" + std::to_string(config_.port);
}

RendezvousServerSnapshot RendezvousServer::Snapshot() const {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    return snapshot_;
}

void RendezvousServer::UpdateSnapshot(RendezvousRegistry* registry) {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    if (registry != nullptr) {
        snapshot_.rooms = registry->Snapshot(std::chrono::steady_clock::now());
        snapshot_.relay = registry->RelaySnapshot();
    }
}

int RendezvousServer::Run() {
    UdpEndpoint local{};
    if (!ParseUdpEndpoint(config_.bindAddress, config_.port, &local)
        || local.family != AF_INET) {
        {
            std::lock_guard<std::mutex> lock(snapshotMutex_);
            snapshot_.lastError = "bind_address must be an IPv4 address";
        }
        Log(LogLevel::Error, "bind_address must be an IPv4 address");
        return 2;
    }

    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSocket
        || bind(sock, reinterpret_cast<const sockaddr*>(&local.addr), local.addr_len) != 0) {
        const int socketError = GetSocketError();
        Log(LogLevel::Error, "Cannot bind " + config_.bindAddress + ":"
            + std::to_string(config_.port) + ", error="
            + std::to_string(socketError));
        {
            std::lock_guard<std::mutex> lock(snapshotMutex_);
            snapshot_.lastError = "Cannot bind endpoint, socket error="
                + std::to_string(socketError);
        }
        CloseSocket(sock);
        return 1;
    }
    SetSocketRecvTimeoutMs(sock, 1000);
    Log(LogLevel::Info, "EasyTunnel rendezvous listening on "
        + config_.bindAddress + ":" + std::to_string(config_.port) + " (IPv4/UDP)");
    Log(LogLevel::Info, "Room capacity=" + std::to_string(config_.maxClientsPerRoom)
        + ", client timeout=" + std::to_string(config_.clientTimeoutSeconds) + "s");
    Log(LogLevel::Info, "IPv4 relay="
        + std::string(config_.ipv4RelayEnabled ? "enabled" : "disabled")
        + ", UDP ports=" + std::to_string(config_.ipv4RelayPortStart)
        + "-" + std::to_string(config_.ipv4RelayPortEnd));

    RendezvousRegistry registry(sock, config_);
    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        snapshot_.listening = true;
        snapshot_.lastError.clear();
    }
    std::vector<uint8_t> buffer(2048);
    while (running_.load()) {
        sockaddr_storage sourceAddress{};
        socket_len_t sourceLen = static_cast<socket_len_t>(sizeof(sourceAddress));
        const int n = recvfrom(sock, reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&sourceAddress), &sourceLen);
        if (n < 0) {
            if (IsRecvTimeout(GetSocketError())) {
                UpdateSnapshot(&registry);
                continue;
            }
            Log(LogLevel::Error, "recvfrom failed. error="
                + std::to_string(GetSocketError()));
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(snapshotMutex_);
            ++snapshot_.receivedDatagrams;
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
        {
            std::lock_guard<std::mutex> lock(snapshotMutex_);
            ++snapshot_.controlMessages;
        }
        registry.Handle(source, type, fields, std::chrono::steady_clock::now());
        UpdateSnapshot(&registry);
    }

    UpdateSnapshot(&registry);
    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        snapshot_.listening = false;
    }
    CloseSocket(sock);
    Log(LogLevel::Info, "EasyTunnel rendezvous stopped");
    return 0;
}
