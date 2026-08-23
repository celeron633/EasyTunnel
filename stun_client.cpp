#include "stun_client.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#include "secure_random.h"

namespace {
constexpr uint16_t kBindingRequest = 0x0001;
constexpr uint16_t kBindingSuccessResponse = 0x0101;
constexpr uint16_t kBindingErrorResponse = 0x0111;
constexpr uint16_t kXorMappedAddress = 0x0020;
constexpr uint32_t kMagicCookie = 0x2112A442;
constexpr size_t kHeaderSize = 20;
constexpr size_t kMaxStunPacketSize = 2048;

uint16_t ReadUint16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8)
                                 | static_cast<uint16_t>(data[1]));
}

uint32_t ReadUint32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24)
        | (static_cast<uint32_t>(data[1]) << 16)
        | (static_cast<uint32_t>(data[2]) << 8)
        | static_cast<uint32_t>(data[3]);
}

void WriteUint16(uint8_t* data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value >> 8);
    data[1] = static_cast<uint8_t>(value);
}

void WriteUint32(uint8_t* data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value >> 24);
    data[1] = static_cast<uint8_t>(value >> 16);
    data[2] = static_cast<uint8_t>(value >> 8);
    data[3] = static_cast<uint8_t>(value);
}

void SetError(std::string* error, const std::string& value) {
    if (error != nullptr) *error = value;
}

UdpEndpoint FromSockaddr(const sockaddr_storage& address, socket_len_t len) {
    UdpEndpoint endpoint{};
    endpoint.addr = address;
    endpoint.addr_len = len;
    endpoint.family = address.ss_family;
    return endpoint;
}

bool HasTransactionId(const uint8_t* data, size_t len,
                      const StunTransactionId& transactionId) {
    return len >= kHeaderSize && ReadUint32(data + 4) == kMagicCookie
        && std::equal(transactionId.begin(), transactionId.end(), data + 8);
}

bool ParseXorMappedAddress(const uint8_t* value, size_t len,
                           const StunTransactionId& transactionId,
                           UdpEndpoint* endpoint, std::string* error) {
    if (len < 4 || value[0] != 0) {
        SetError(error, "Malformed STUN XOR-MAPPED-ADDRESS");
        return false;
    }

    const uint16_t port = static_cast<uint16_t>(ReadUint16(value + 2)
                                                 ^ (kMagicCookie >> 16));
    if (port == 0) {
        SetError(error, "STUN returned an invalid mapped port");
        return false;
    }

    if (value[1] == 0x01) {
        if (len != 8) {
            SetError(error, "Malformed IPv4 STUN XOR-MAPPED-ADDRESS");
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        const std::array<uint8_t, 4> cookieBytes{
            0x21, 0x12, 0xA4, 0x42,
        };
        uint8_t decoded[4]{};
        for (size_t i = 0; i < sizeof(decoded); ++i) {
            decoded[i] = static_cast<uint8_t>(value[4 + i] ^ cookieBytes[i]);
        }
        std::memcpy(&address.sin_addr, decoded, sizeof(decoded));
        std::memcpy(&endpoint->addr, &address, sizeof(address));
        endpoint->addr_len = static_cast<socket_len_t>(sizeof(address));
        endpoint->family = AF_INET;
        return true;
    }

    if (value[1] == 0x02) {
        if (len != 20) {
            SetError(error, "Malformed IPv6 STUN XOR-MAPPED-ADDRESS");
            return false;
        }
        const std::array<uint8_t, 4> cookieBytes{
            0x21, 0x12, 0xA4, 0x42,
        };
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(port);
        uint8_t* decoded = reinterpret_cast<uint8_t*>(&address.sin6_addr);
        for (size_t i = 0; i < 4; ++i) {
            decoded[i] = static_cast<uint8_t>(value[4 + i] ^ cookieBytes[i]);
        }
        for (size_t i = 4; i < 16; ++i) {
            decoded[i] = static_cast<uint8_t>(
                value[4 + i] ^ transactionId[i - 4]);
        }
        std::memcpy(&endpoint->addr, &address, sizeof(address));
        endpoint->addr_len = static_cast<socket_len_t>(sizeof(address));
        endpoint->family = AF_INET6;
        return true;
    }

    SetError(error, "STUN returned an unknown address family");
    return false;
}

bool Ipv4AddressAndPort(const UdpEndpoint& endpoint, uint32_t* address,
                        uint16_t* port) {
    if (endpoint.family != AF_INET
        || endpoint.addr_len < static_cast<socket_len_t>(sizeof(sockaddr_in))) {
        return false;
    }
    const auto* value = reinterpret_cast<const sockaddr_in*>(&endpoint.addr);
    *address = value->sin_addr.s_addr;
    *port = ntohs(value->sin_port);
    return true;
}

bool SameIpAddress(const UdpEndpoint& first, const UdpEndpoint& second) {
    uint32_t firstAddress = 0;
    uint32_t secondAddress = 0;
    uint16_t ignoredPort = 0;
    return Ipv4AddressAndPort(first, &firstAddress, &ignoredPort)
        && Ipv4AddressAndPort(second, &secondAddress, &ignoredPort)
        && firstAddress == secondAddress;
}
}  // namespace

bool GenerateStunTransactionId(StunTransactionId* transactionId,
                               std::string* error) {
    return transactionId != nullptr
        && FillSecureRandom(transactionId->data(), transactionId->size(), error);
}

std::vector<uint8_t> MakeStunBindingRequest(
    const StunTransactionId& transactionId) {
    std::vector<uint8_t> request(kHeaderSize, 0);
    WriteUint16(request.data(), kBindingRequest);
    WriteUint16(request.data() + 2, 0);
    WriteUint32(request.data() + 4, kMagicCookie);
    std::copy(transactionId.begin(), transactionId.end(), request.begin() + 8);
    return request;
}

bool ParseStunBindingResponse(const uint8_t* data, size_t len,
                              const StunTransactionId& transactionId,
                              UdpEndpoint* mappedEndpoint,
                              std::string* error) {
    if (data == nullptr || mappedEndpoint == nullptr || len < kHeaderSize) {
        SetError(error, "STUN response is shorter than its header");
        return false;
    }
    const uint16_t messageType = ReadUint16(data);
    const uint16_t messageLength = ReadUint16(data + 2);
    if ((messageType & 0xC000) != 0 || (messageLength & 0x0003) != 0) {
        SetError(error, "Malformed STUN response header");
        return false;
    }
    if (ReadUint32(data + 4) != kMagicCookie) {
        SetError(error, "STUN magic cookie does not match");
        return false;
    }
    if (!std::equal(transactionId.begin(), transactionId.end(), data + 8)) {
        SetError(error, "STUN transaction ID does not match");
        return false;
    }
    if (kHeaderSize + messageLength > len) {
        SetError(error, "Truncated STUN response");
        return false;
    }
    if (messageType == kBindingErrorResponse) {
        SetError(error, "STUN server rejected the Binding request");
        return false;
    }
    if (messageType != kBindingSuccessResponse) {
        SetError(error, "Unexpected STUN message type");
        return false;
    }

    const size_t end = kHeaderSize + messageLength;
    for (size_t offset = kHeaderSize; offset < end;) {
        if (end - offset < 4) {
            SetError(error, "Truncated STUN attribute header");
            return false;
        }
        const uint16_t attributeType = ReadUint16(data + offset);
        const uint16_t attributeLength = ReadUint16(data + offset + 2);
        const size_t paddedLength = (static_cast<size_t>(attributeLength) + 3)
            & ~static_cast<size_t>(3);
        if (paddedLength > end - offset - 4) {
            SetError(error, "Truncated STUN attribute");
            return false;
        }
        if (attributeType == kXorMappedAddress) {
            return ParseXorMappedAddress(data + offset + 4, attributeLength,
                                         transactionId, mappedEndpoint, error);
        }
        offset += 4 + paddedLength;
    }

    SetError(error, "STUN response has no XOR-MAPPED-ADDRESS");
    return false;
}

bool QueryStunBinding(socket_t sock, const StunServerConfig& server,
                      int timeoutMs, int attempts, StunProbeResult* result,
                      std::string* error) {
    if (sock == kInvalidSocket || result == nullptr || server.host.empty()
        || server.port == 0 || timeoutMs <= 0 || attempts <= 0) {
        SetError(error, "Invalid STUN query arguments");
        return false;
    }

    UdpEndpoint serverEndpoint{};
    if (!ResolveUdpEndpoint(server.host, server.port, AF_INET,
                            &serverEndpoint, error)) {
        return false;
    }

    std::string lastError = "STUN server did not respond";
    for (int attempt = 0; attempt < attempts; ++attempt) {
        StunTransactionId transactionId{};
        if (!GenerateStunTransactionId(&transactionId, error)) return false;
        const std::vector<uint8_t> request =
            MakeStunBindingRequest(transactionId);
        const int sent = sendto(sock,
            reinterpret_cast<const char*>(request.data()),
            static_cast<int>(request.size()), 0,
            reinterpret_cast<const sockaddr*>(&serverEndpoint.addr),
            serverEndpoint.addr_len);
        if (sent != static_cast<int>(request.size())) {
            lastError = "Cannot send STUN Binding request. err="
                + std::to_string(GetSocketError());
            continue;
        }

        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<
                std::chrono::milliseconds>(deadline
                    - std::chrono::steady_clock::now()).count();
            SetSocketRecvTimeoutMs(sock,
                static_cast<int>((std::max)(int64_t{1}, remaining)));

            std::array<uint8_t, kMaxStunPacketSize> buffer{};
            sockaddr_storage sourceAddress{};
            socket_len_t sourceLen =
                static_cast<socket_len_t>(sizeof(sourceAddress));
            const int received = recvfrom(sock,
                reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()), 0,
                reinterpret_cast<sockaddr*>(&sourceAddress), &sourceLen);
            if (received < 0) {
                const int socketError = GetSocketError();
                if (IsRecvTimeout(socketError)
                    || IsUdpDestinationUnreachable(socketError)) {
                    break;
                }
                SetError(error, "Cannot receive STUN response. err="
                    + std::to_string(socketError));
                return false;
            }

            const UdpEndpoint source = FromSockaddr(sourceAddress, sourceLen);
            if (!SameUdpEndpoint(source, serverEndpoint)
                || !HasTransactionId(buffer.data(),
                                     static_cast<size_t>(received),
                                     transactionId)) {
                continue;
            }

            UdpEndpoint mappedEndpoint{};
            if (!ParseStunBindingResponse(buffer.data(),
                    static_cast<size_t>(received), transactionId,
                    &mappedEndpoint, &lastError)) {
                break;
            }
            result->server = server;
            result->serverEndpoint = serverEndpoint;
            result->mappedEndpoint = mappedEndpoint;
            return true;
        }
    }

    SetError(error, server.host + ":" + std::to_string(server.port)
        + ": " + lastError);
    return false;
}

bool ProbeStunServers(socket_t sock,
                      const std::vector<StunServerConfig>& servers,
                      int timeoutMs, int attempts,
                      std::vector<StunProbeResult>* results,
                      std::string* error) {
    if (results == nullptr || servers.size() < 2) {
        SetError(error, "At least two STUN servers are required");
        return false;
    }
    results->clear();
    results->reserve(servers.size());
    for (const auto& server : servers) {
        StunProbeResult result;
        if (!QueryStunBinding(sock, server, timeoutMs, attempts,
                              &result, error)) {
            results->clear();
            return false;
        }
        if (std::any_of(results->begin(), results->end(),
                [&result](const StunProbeResult& previous) {
                    return SameIpAddress(previous.serverEndpoint,
                                         result.serverEndpoint);
                })) {
            SetError(error,
                "STUN servers must resolve to different public IPv4 addresses");
            results->clear();
            return false;
        }
        results->push_back(std::move(result));
    }
    return true;
}

bool DiagnoseStunServers(const std::vector<StunServerConfig>& servers,
                         int timeoutMs, int attempts,
                         StunDiagnosticResult* result,
                         std::string* error) {
    if (result == nullptr || servers.size() < 2) {
        SetError(error, "At least two STUN servers are required");
        return false;
    }
    *result = {};

    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSocket) {
        SetError(error, "Cannot create STUN diagnostic socket. err="
            + std::to_string(GetSocketError()));
        return false;
    }

    const bool probed = ProbeStunServers(sock, servers, timeoutMs, attempts,
                                         &result->probes, error);
    CloseSocket(sock);
    if (!probed) return false;

    result->mapping = ClassifyNatMapping(
        result->probes[0].mappedEndpoint,
        result->probes[1].mappedEndpoint);
    return true;
}

std::string FormatStunDiagnosticSummary(
    const StunDiagnosticResult& result) {
    if (result.probes.size() < 2) return "STUN diagnostic has no result";
    const char* localPlan = "unsupported";
    switch (result.mapping.behavior) {
        case NatMappingBehavior::EndpointIndependent:
            localPlan = "direct-or-range-ready";
            break;
        case NatMappingBehavior::PortDependentRegular:
            localPlan = "regular-or-dual-range-ready";
            break;
        case NatMappingBehavior::PortDependentRandom:
            localPlan = "random-required-pending";
            break;
        default:
            break;
    }
    return std::string("classification=")
        + NatMappingBehaviorName(result.mapping.behavior)
        + "; A=" + FormatUdpEndpoint(result.probes[0].mappedEndpoint)
        + "; B=" + FormatUdpEndpoint(result.probes[1].mappedEndpoint)
        + "; delta=" + std::to_string(result.mapping.portDelta)
        + "; local_plan=" + localPlan;
}

NatMappingAnalysis ClassifyNatMapping(const UdpEndpoint& first,
                                      const UdpEndpoint& second,
                                      uint16_t regularDeltaLimit) {
    uint32_t firstAddress = 0;
    uint32_t secondAddress = 0;
    uint16_t firstPort = 0;
    uint16_t secondPort = 0;
    if (!Ipv4AddressAndPort(first, &firstAddress, &firstPort)
        || !Ipv4AddressAndPort(second, &secondAddress, &secondPort)) {
        return {};
    }
    if (firstAddress != secondAddress) {
        return {NatMappingBehavior::MultiPublicIp, 0};
    }

    const int delta = static_cast<int>(secondPort)
        - static_cast<int>(firstPort);
    if (delta == 0) {
        return {NatMappingBehavior::EndpointIndependent, 0};
    }
    const int absoluteDelta = delta < 0 ? -delta : delta;
    if (absoluteDelta <= static_cast<int>(regularDeltaLimit)) {
        return {NatMappingBehavior::PortDependentRegular, delta};
    }
    return {NatMappingBehavior::PortDependentRandom, delta};
}

const char* NatMappingBehaviorName(NatMappingBehavior behavior) {
    switch (behavior) {
        case NatMappingBehavior::EndpointIndependent:
            return "endpoint-independent";
        case NatMappingBehavior::PortDependentRegular:
            return "port-dependent-regular";
        case NatMappingBehavior::PortDependentRandom:
            return "port-dependent-random";
        case NatMappingBehavior::MultiPublicIp:
            return "multi-public-ip";
        default:
            return "unknown";
    }
}

bool ParseNatMappingBehavior(const std::string& text,
                             NatMappingBehavior* behavior) {
    if (behavior == nullptr) return false;
    if (text == "endpoint-independent") {
        *behavior = NatMappingBehavior::EndpointIndependent;
    } else if (text == "port-dependent-regular") {
        *behavior = NatMappingBehavior::PortDependentRegular;
    } else if (text == "port-dependent-random") {
        *behavior = NatMappingBehavior::PortDependentRandom;
    } else if (text == "multi-public-ip") {
        *behavior = NatMappingBehavior::MultiPublicIp;
    } else if (text == "unknown") {
        *behavior = NatMappingBehavior::Unknown;
    } else {
        return false;
    }
    return true;
}
