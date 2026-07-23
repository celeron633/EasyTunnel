#include "ipv4_relay_app.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../log.h"
#include "../nat_protocol.h"

namespace {
constexpr int kRelayRecvTimeoutMs = 500;
constexpr size_t kRelayBufferSize = 65535;

bool Send(socket_t sock, const UdpEndpoint& endpoint, const std::string& data) {
    return sendto(sock, data.data(), static_cast<int>(data.size()), 0,
        reinterpret_cast<const sockaddr*>(&endpoint.addr), endpoint.addr_len)
        == static_cast<int>(data.size());
}

bool Send(socket_t sock, const UdpEndpoint& endpoint,
          const uint8_t* data, size_t size) {
    return sendto(sock, reinterpret_cast<const char*>(data),
        static_cast<int>(size), 0,
        reinterpret_cast<const sockaddr*>(&endpoint.addr), endpoint.addr_len)
        == static_cast<int>(size);
}

void SendMessage(socket_t sock, const UdpEndpoint& endpoint,
                 const std::string& type,
                 const std::vector<std::string>& fields = {}) {
    Send(sock, endpoint, MakeControlMessage(type, fields));
}

UdpEndpoint FromSockaddr(const sockaddr_storage& address, socket_len_t len) {
    UdpEndpoint endpoint{};
    endpoint.addr = address;
    endpoint.addr_len = len;
    endpoint.family = address.ss_family;
    return endpoint;
}

std::string RandomHex(size_t bytes) {
    std::random_device random;
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (size_t i = 0; i < bytes; ++i) {
        output << std::setw(2) << (random() & 0xffU);
    }
    return output.str();
}

std::string PairKey(const std::string& roomId, const std::string& first,
                    const std::string& second) {
    return roomId + '\x1f' + ((first < second)
        ? first + '\x1f' + second : second + '\x1f' + first);
}

struct RelaySide {
    std::string nodeId;
    std::string accessKey;
    UdpEndpoint endpoint{};
    bool bound = false;
    std::chrono::steady_clock::time_point lastSeen{};
};

struct RelayCounters {
    std::atomic<uint64_t> receivedDatagrams{0};
    std::atomic<uint64_t> forwardedDatagrams{0};
    std::atomic<uint64_t> forwardedBytes{0};
};

struct RelaySession {
    std::string roomId;
    std::string sessionId;
    uint16_t port = 0;
    socket_t sock = kInvalidSocket;
    RelaySide sides[2];
    std::chrono::steady_clock::time_point created{};
    uint16_t timeoutSeconds = 60;
    std::atomic<bool> running{true};
    std::atomic<bool> finished{false};
    std::thread worker;
    RelayCounters* counters = nullptr;
};

int SideIndex(const RelaySession& session, const std::string& nodeId) {
    if (session.sides[0].nodeId == nodeId) return 0;
    if (session.sides[1].nodeId == nodeId) return 1;
    return -1;
}

void SendReady(RelaySession* session, int side) {
    const int other = 1 - side;
    SendMessage(session->sock, session->sides[side].endpoint, "RELAY_READY",
        {session->sessionId, session->sides[other].nodeId});
}

void RelayWorker(RelaySession* session) {
    std::vector<uint8_t> buffer(kRelayBufferSize);
    bool ready = false;
    Log(LogLevel::Info, "IPv4 relay worker started room=" + session->roomId
        + " peer=" + session->sides[0].nodeId
        + " target=" + session->sides[1].nodeId
        + " port=" + std::to_string(session->port));

    while (session->running.load()) {
        sockaddr_storage sourceAddress{};
        socket_len_t sourceLen = static_cast<socket_len_t>(sizeof(sourceAddress));
        const int received = recvfrom(session->sock,
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&sourceAddress), &sourceLen);
        const auto now = std::chrono::steady_clock::now();
        if (received < 0) {
            if (!session->running.load()) break;
            if (!IsRecvTimeout(GetSocketError())) {
                Log(LogLevel::Warn, "IPv4 relay recvfrom failed port="
                    + std::to_string(session->port) + " err="
                    + std::to_string(GetSocketError()));
            }
        } else {
            session->counters->receivedDatagrams.fetch_add(1);
            const UdpEndpoint source = FromSockaddr(sourceAddress, sourceLen);
            std::string type;
            std::vector<std::string> fields;
            const bool control = ParseControlMessage(
                buffer.data(), static_cast<size_t>(received), &type, &fields);
            if (control && type == "RELAY_HELLO" && fields.size() == 3
                && fields[0] == session->sessionId) {
                const int side = SideIndex(*session, fields[1]);
                if (side >= 0 && fields[2] == session->sides[side].accessKey) {
                    session->sides[side].endpoint = source;
                    session->sides[side].bound = true;
                    session->sides[side].lastSeen = now;
                    if (session->sides[0].bound && session->sides[1].bound) {
                        if (!ready) {
                            ready = true;
                            Log(LogLevel::Info, "IPv4 relay ready room="
                                + session->roomId + " peer="
                                + session->sides[0].nodeId + " endpoint="
                                + FormatUdpEndpoint(session->sides[0].endpoint)
                                + " target=" + session->sides[1].nodeId
                                + " endpoint="
                                + FormatUdpEndpoint(session->sides[1].endpoint)
                                + " port=" + std::to_string(session->port));
                        }
                        SendReady(session, side);
                        SendReady(session, 1 - side);
                    }
                }
            } else if (ready) {
                int side = -1;
                if (SameUdpEndpoint(source, session->sides[0].endpoint)) side = 0;
                else if (SameUdpEndpoint(source, session->sides[1].endpoint)) side = 1;
                if (side >= 0) {
                    session->sides[side].lastSeen = now;
                    const int other = 1 - side;
                    if (Send(session->sock, session->sides[other].endpoint,
                             buffer.data(), static_cast<size_t>(received))) {
                        session->counters->forwardedDatagrams.fetch_add(1);
                        session->counters->forwardedBytes.fetch_add(
                            static_cast<uint64_t>(received));
                    }
                }
            }
        }

        if (!ready) {
            if (now - session->created
                > std::chrono::seconds(session->timeoutSeconds)) break;
        } else if (now - session->sides[0].lastSeen
                       > std::chrono::seconds(session->timeoutSeconds)
                   || now - session->sides[1].lastSeen
                       > std::chrono::seconds(session->timeoutSeconds)) {
            break;
        }
    }

    session->finished.store(true);
    Log(LogLevel::Info, "IPv4 relay worker stopped room=" + session->roomId
        + " peer=" + session->sides[0].nodeId
        + " target=" + session->sides[1].nodeId
        + " port=" + std::to_string(session->port));
}
}  // namespace

struct Ipv4RelayApp::Impl {
    Impl(socket_t socket, const RendezvousConfig& settings)
        : controlSocket(socket), config(settings) {}

    ~Impl() { StopAll(); }

    socket_t controlSocket;
    RendezvousConfig config;
    RelayCounters counters;
    std::unordered_map<std::string, std::unique_ptr<RelaySession>> sessions;

    void StopSession(RelaySession* session) {
        session->running.store(false);
        ShutdownSocket(session->sock);
        if (session->worker.joinable()) session->worker.join();
        CloseSocket(session->sock);
    }

    void StopAll() {
        for (auto& entry : sessions) {
            entry.second->running.store(false);
            ShutdownSocket(entry.second->sock);
        }
        for (auto& entry : sessions) {
            if (entry.second->worker.joinable()) entry.second->worker.join();
            CloseSocket(entry.second->sock);
        }
        sessions.clear();
    }

    void CleanupFinished() {
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (!it->second->finished.load()) {
                ++it;
                continue;
            }
            StopSession(it->second.get());
            it = sessions.erase(it);
        }
    }

    bool BindRelaySocket(socket_t* output, uint16_t* port) const {
        for (uint32_t candidate = config.ipv4RelayPortStart;
             candidate <= config.ipv4RelayPortEnd; ++candidate) {
            socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock == kInvalidSocket) return false;
            UdpEndpoint endpoint{};
            if (ParseUdpEndpoint(config.bindAddress,
                                 static_cast<uint16_t>(candidate), &endpoint)
                && endpoint.family == AF_INET
                && bind(sock, reinterpret_cast<const sockaddr*>(&endpoint.addr),
                        endpoint.addr_len) == 0) {
                SetSocketRecvTimeoutMs(sock, kRelayRecvTimeoutMs);
                *output = sock;
                *port = static_cast<uint16_t>(candidate);
                return true;
            }
            CloseSocket(sock);
        }
        return false;
    }

    void SendOffer(const RelaySession& session, const UdpEndpoint& source,
                   const std::string& nodeId, const std::string& targetId) const {
        const int side = SideIndex(session, nodeId);
        if (side < 0) return;
        SendMessage(controlSocket, source, "RELAY_OFFER",
            {std::to_string(session.port), targetId, session.sessionId,
             session.sides[side].accessKey});
    }
};

Ipv4RelayApp::Ipv4RelayApp(socket_t controlSocket,
                           const RendezvousConfig& config)
    : impl_(std::make_unique<Impl>(controlSocket, config)) {}

Ipv4RelayApp::~Ipv4RelayApp() = default;

void Ipv4RelayApp::HandleJoin(
    const UdpEndpoint& source, const std::string& roomId,
    const std::string& nodeId, const std::string& targetId,
    std::chrono::steady_clock::time_point now) {
    impl_->CleanupFinished();
    if (!impl_->config.ipv4RelayEnabled) {
        SendMessage(impl_->controlSocket, source, "ERROR", {"ipv4-relay-disabled"});
        return;
    }

    const std::string key = PairKey(roomId, nodeId, targetId);
    auto existing = impl_->sessions.find(key);
    if (existing != impl_->sessions.end()) {
        impl_->SendOffer(*existing->second, source, nodeId, targetId);
        return;
    }

    socket_t relaySocket = kInvalidSocket;
    uint16_t relayPort = 0;
    if (!impl_->BindRelaySocket(&relaySocket, &relayPort)) {
        SendMessage(impl_->controlSocket, source, "ERROR",
                    {"ipv4-relay-port-exhausted"});
        Log(LogLevel::Warn, "Cannot allocate IPv4 relay port for room="
            + roomId + " peer=" + nodeId + " target=" + targetId);
        return;
    }

    RelaySession* started = nullptr;
    try {
        auto session = std::make_unique<RelaySession>();
        session->roomId = roomId;
        session->sessionId = RandomHex(16);
        session->port = relayPort;
        session->sock = relaySocket;
        session->created = now;
        session->timeoutSeconds = impl_->config.clientTimeoutSeconds;
        session->counters = &impl_->counters;
        if (nodeId < targetId) {
            session->sides[0].nodeId = nodeId;
            session->sides[1].nodeId = targetId;
        } else {
            session->sides[0].nodeId = targetId;
            session->sides[1].nodeId = nodeId;
        }
        session->sides[0].accessKey = RandomHex(16);
        session->sides[1].accessKey = RandomHex(16);
        started = session.get();
        impl_->sessions.emplace(key, std::move(session));
    } catch (...) {
        CloseSocket(relaySocket);
        SendMessage(impl_->controlSocket, source, "ERROR",
                    {"ipv4-relay-resource-unavailable"});
        Log(LogLevel::Error, "Cannot create IPv4 relay worker for room="
            + roomId + " peer=" + nodeId + " target=" + targetId);
        return;
    }
    try {
        started->worker = std::thread(RelayWorker, started);
    } catch (...) {
        CloseSocket(started->sock);
        impl_->sessions.erase(key);
        SendMessage(impl_->controlSocket, source, "ERROR",
                    {"ipv4-relay-resource-unavailable"});
        Log(LogLevel::Error, "Cannot start IPv4 relay worker for room="
            + roomId + " peer=" + nodeId + " target=" + targetId);
        return;
    }
    impl_->SendOffer(*started, source, nodeId, targetId);
}

void Ipv4RelayApp::RemovePeer(const std::string& roomId,
                              const std::string& nodeId) {
    impl_->CleanupFinished();
    for (auto it = impl_->sessions.begin(); it != impl_->sessions.end();) {
        RelaySession* session = it->second.get();
        if (session->roomId != roomId || SideIndex(*session, nodeId) < 0) {
            ++it;
            continue;
        }
        impl_->StopSession(session);
        it = impl_->sessions.erase(it);
    }
}

Ipv4RelayAppSnapshot Ipv4RelayApp::Snapshot() {
    impl_->CleanupFinished();
    Ipv4RelayAppSnapshot snapshot;
    snapshot.activeSessions = static_cast<uint64_t>(impl_->sessions.size());
    snapshot.receivedDatagrams = impl_->counters.receivedDatagrams.load();
    snapshot.forwardedDatagrams = impl_->counters.forwardedDatagrams.load();
    snapshot.forwardedBytes = impl_->counters.forwardedBytes.load();
    return snapshot;
}
