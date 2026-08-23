#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "stun_client.h"

namespace {
constexpr uint32_t kMagicCookie = 0x2112A442;
int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
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

std::vector<uint8_t> MakeIpv4Response(const StunTransactionId& transactionId,
                                      const std::array<uint8_t, 4>& address,
                                      uint16_t port) {
    std::vector<uint8_t> response(32, 0);
    WriteUint16(response.data(), 0x0101);
    WriteUint16(response.data() + 2, 12);
    WriteUint32(response.data() + 4, kMagicCookie);
    std::memcpy(response.data() + 8, transactionId.data(),
                transactionId.size());
    WriteUint16(response.data() + 20, 0x8022);  // SOFTWARE, ignored.
    WriteUint16(response.data() + 22, 0);
    WriteUint16(response.data() + 24, 0x0020);
    WriteUint16(response.data() + 26, 8);
    response[28] = 0;
    response[29] = 0x01;
    WriteUint16(response.data() + 30,
                static_cast<uint16_t>(port ^ (kMagicCookie >> 16)));
    const std::array<uint8_t, 4> cookieBytes{0x21, 0x12, 0xA4, 0x42};
    for (size_t i = 0; i < address.size(); ++i) {
        response.push_back(static_cast<uint8_t>(address[i] ^ cookieBytes[i]));
    }
    WriteUint16(response.data() + 2, 16);
    return response;
}

UdpEndpoint Endpoint(const char* ip, uint16_t port) {
    UdpEndpoint endpoint{};
    ParseUdpEndpoint(ip, port, &endpoint);
    return endpoint;
}
}  // namespace

int main() {
#ifdef _WIN32
    WSADATA winsockData{};
    if (WSAStartup(MAKEWORD(2, 2), &winsockData) != 0) {
        std::cerr << "FAILED: Winsock initialization\n";
        return 1;
    }
#endif
    const StunTransactionId transactionId{
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
        0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    };
    const auto request = MakeStunBindingRequest(transactionId);
    Expect(request.size() == 20, "Binding request has an RFC 8489 header");
    Expect(request[0] == 0 && request[1] == 1
               && request[2] == 0 && request[3] == 0,
           "Binding request type and length");
    Expect(request[4] == 0x21 && request[5] == 0x12
               && request[6] == 0xA4 && request[7] == 0x42,
           "Binding request magic cookie");
    Expect(std::equal(transactionId.begin(), transactionId.end(),
                      request.begin() + 8),
           "Binding request transaction ID");

    auto response = MakeIpv4Response(transactionId, {203, 0, 113, 7}, 45678);
    UdpEndpoint mapped{};
    std::string error;
    Expect(ParseStunBindingResponse(response.data(), response.size(),
                                    transactionId, &mapped, &error),
           "valid XOR-MAPPED-ADDRESS parses");
    Expect(FormatUdpEndpoint(mapped) == "203.0.113.7:45678",
           "XOR-MAPPED-ADDRESS decodes IPv4 and port");

    auto wrongTransaction = transactionId;
    wrongTransaction[0] ^= 0xFF;
    Expect(!ParseStunBindingResponse(response.data(), response.size(),
                                     wrongTransaction, &mapped, &error),
           "wrong transaction ID is rejected");

    auto wrongCookie = response;
    wrongCookie[4] ^= 0x01;
    Expect(!ParseStunBindingResponse(wrongCookie.data(), wrongCookie.size(),
                                     transactionId, &mapped, &error),
           "wrong magic cookie is rejected");

    auto truncated = response;
    truncated.resize(truncated.size() - 1);
    Expect(!ParseStunBindingResponse(truncated.data(), truncated.size(),
                                     transactionId, &mapped, &error),
           "truncated attribute is rejected");

    auto missingAddress = response;
    WriteUint16(missingAddress.data() + 24, 0x8023);
    Expect(!ParseStunBindingResponse(missingAddress.data(),
                                     missingAddress.size(), transactionId,
                                     &mapped, &error),
           "response without XOR-MAPPED-ADDRESS is rejected");

    const auto stable = ClassifyNatMapping(
        Endpoint("198.51.100.20", 40000),
        Endpoint("198.51.100.20", 40000));
    Expect(stable.behavior == NatMappingBehavior::EndpointIndependent
               && stable.portDelta == 0,
           "stable mapping classification");

    const auto regular = ClassifyNatMapping(
        Endpoint("198.51.100.20", 40000),
        Endpoint("198.51.100.20", 40003));
    Expect(regular.behavior == NatMappingBehavior::PortDependentRegular
               && regular.portDelta == 3,
           "regular port-dependent mapping classification");

    const auto decreasing = ClassifyNatMapping(
        Endpoint("198.51.100.20", 40000),
        Endpoint("198.51.100.20", 39998));
    Expect(decreasing.behavior == NatMappingBehavior::PortDependentRegular
               && decreasing.portDelta == -2,
           "decreasing regular mapping classification");

    const auto random = ClassifyNatMapping(
        Endpoint("198.51.100.20", 40000),
        Endpoint("198.51.100.20", 40100));
    Expect(random.behavior == NatMappingBehavior::PortDependentRandom,
           "random port-dependent mapping classification");

    const auto multiIp = ClassifyNatMapping(
        Endpoint("198.51.100.20", 40000),
        Endpoint("198.51.100.21", 40001));
    Expect(multiIp.behavior == NatMappingBehavior::MultiPublicIp,
           "multiple public IP classification");

    socket_t fakeServer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    Expect(fakeServer != kInvalidSocket, "fake STUN server socket opens");
    if (fakeServer != kInvalidSocket) {
        sockaddr_in bindAddress{};
        bindAddress.sin_family = AF_INET;
        bindAddress.sin_port = 0;
        bindAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const int bound = bind(fakeServer,
            reinterpret_cast<const sockaddr*>(&bindAddress),
            static_cast<socket_len_t>(sizeof(bindAddress)));
        Expect(bound == 0, "fake STUN server binds");
        SetSocketRecvTimeoutMs(fakeServer, 1000);

        socket_len_t bindAddressLen =
            static_cast<socket_len_t>(sizeof(bindAddress));
        const int named = getsockname(fakeServer,
            reinterpret_cast<sockaddr*>(&bindAddress), &bindAddressLen);
        Expect(named == 0, "fake STUN server discovers its port");

        bool fakeServerHandledRequest = false;
        std::thread fakeServerThread([&] {
            std::array<uint8_t, 256> buffer{};
            sockaddr_storage clientAddress{};
            socket_len_t clientAddressLen =
                static_cast<socket_len_t>(sizeof(clientAddress));
            const int received = recvfrom(fakeServer,
                reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()), 0,
                reinterpret_cast<sockaddr*>(&clientAddress),
                &clientAddressLen);
            if (received != 20) return;
            StunTransactionId receivedTransaction{};
            std::copy(buffer.begin() + 8, buffer.begin() + 20,
                      receivedTransaction.begin());
            const auto reply = MakeIpv4Response(
                receivedTransaction, {198, 51, 100, 25}, 45679);
            fakeServerHandledRequest = sendto(fakeServer,
                reinterpret_cast<const char*>(reply.data()),
                static_cast<int>(reply.size()), 0,
                reinterpret_cast<const sockaddr*>(&clientAddress),
                clientAddressLen) == static_cast<int>(reply.size());
        });

        socket_t client = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        Expect(client != kInvalidSocket, "STUN client socket opens");
        if (client != kInvalidSocket && bound == 0 && named == 0) {
            StunProbeResult probe;
            const StunServerConfig server{
                "127.0.0.1", ntohs(bindAddress.sin_port),
            };
            Expect(QueryStunBinding(client, server, 500, 1, &probe, &error),
                   "Binding query works against a UDP STUN endpoint");
            Expect(FormatUdpEndpoint(probe.mappedEndpoint)
                       == "198.51.100.25:45679",
                   "Binding query returns the decoded mapping");
        }
        fakeServerThread.join();
        Expect(fakeServerHandledRequest,
               "fake STUN server handled the Binding request");
        CloseSocket(client);
        CloseSocket(fakeServer);
    }

    socket_t diagnosticServerA = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    socket_t diagnosticServerB = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    Expect(diagnosticServerA != kInvalidSocket
               && diagnosticServerB != kInvalidSocket,
           "dual diagnostic STUN sockets open");
    if (diagnosticServerA != kInvalidSocket
        && diagnosticServerB != kInvalidSocket) {
        auto bindFakeServer = [](socket_t server, const char* ip,
                                 sockaddr_in* address) {
            *address = {};
            address->sin_family = AF_INET;
            address->sin_port = 0;
            if (!ParseIpv4(ip, &address->sin_addr)) return false;
            if (bind(server, reinterpret_cast<const sockaddr*>(address),
                     static_cast<socket_len_t>(sizeof(*address))) != 0) {
                return false;
            }
            SetSocketRecvTimeoutMs(server, 1000);
            socket_len_t addressLen =
                static_cast<socket_len_t>(sizeof(*address));
            return getsockname(server, reinterpret_cast<sockaddr*>(address),
                               &addressLen) == 0;
        };

        sockaddr_in diagnosticAddressA{};
        sockaddr_in diagnosticAddressB{};
        const bool diagnosticBoundA = bindFakeServer(
            diagnosticServerA, "127.0.0.1", &diagnosticAddressA);
        const bool diagnosticBoundB = bindFakeServer(
            diagnosticServerB, "127.0.0.2", &diagnosticAddressB);
        Expect(diagnosticBoundA && diagnosticBoundB,
               "dual diagnostic STUN sockets bind to different IPs");

        bool diagnosticHandledA = false;
        bool diagnosticHandledB = false;
        auto serveBinding = [](socket_t server, uint16_t mappedPort,
                               bool* handled) {
            std::array<uint8_t, 256> buffer{};
            sockaddr_storage clientAddress{};
            socket_len_t clientAddressLen =
                static_cast<socket_len_t>(sizeof(clientAddress));
            const int received = recvfrom(server,
                reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()), 0,
                reinterpret_cast<sockaddr*>(&clientAddress),
                &clientAddressLen);
            if (received != 20) return;
            StunTransactionId receivedTransaction{};
            std::copy(buffer.begin() + 8, buffer.begin() + 20,
                      receivedTransaction.begin());
            const auto reply = MakeIpv4Response(
                receivedTransaction, {198, 51, 100, 30}, mappedPort);
            *handled = sendto(server,
                reinterpret_cast<const char*>(reply.data()),
                static_cast<int>(reply.size()), 0,
                reinterpret_cast<const sockaddr*>(&clientAddress),
                clientAddressLen) == static_cast<int>(reply.size());
        };

        if (diagnosticBoundA && diagnosticBoundB) {
            std::thread serverAThread(
                serveBinding, diagnosticServerA, 40000,
                &diagnosticHandledA);
            std::thread serverBThread(
                serveBinding, diagnosticServerB, 40003,
                &diagnosticHandledB);
            const std::vector<StunServerConfig> diagnosticServers{
                {"127.0.0.1", ntohs(diagnosticAddressA.sin_port)},
                {"127.0.0.2", ntohs(diagnosticAddressB.sin_port)},
            };
            StunDiagnosticResult diagnosticResult;
            Expect(DiagnoseStunServers(diagnosticServers, 500, 1,
                                       &diagnosticResult, &error),
                   "standalone dual-STUN diagnostic succeeds");
            serverAThread.join();
            serverBThread.join();
            Expect(diagnosticHandledA && diagnosticHandledB,
                   "both diagnostic STUN servers handled a request");
            Expect(diagnosticResult.probes.size() == 2,
                   "diagnostic retains both mapped endpoints");
            Expect(diagnosticResult.mapping.behavior
                       == NatMappingBehavior::PortDependentRegular
                       && diagnosticResult.mapping.portDelta == 3,
                   "diagnostic classifies the same-socket port delta");
            Expect(FormatStunDiagnosticSummary(diagnosticResult)
                       == "classification=port-dependent-regular; "
                          "A=198.51.100.30:40000; "
                          "B=198.51.100.30:40003; delta=3; "
                          "local_plan=regular-or-dual-range-ready",
                   "diagnostic summary contains mappings and classification");
        }
        CloseSocket(diagnosticServerA);
        CloseSocket(diagnosticServerB);
    }

    if (failures != 0) {
        std::cerr << failures << " STUN test(s) failed\n";
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    std::cout << "STUN protocol tests passed\n";
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
