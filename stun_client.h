#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "config.h"
#include "util.h"

using StunTransactionId = std::array<uint8_t, 12>;

enum class NatMappingBehavior {
    Unknown,
    EndpointIndependent,
    PortDependentRegular,
    PortDependentRandom,
    MultiPublicIp,
};

struct NatMappingAnalysis {
    NatMappingBehavior behavior = NatMappingBehavior::Unknown;
    int portDelta = 0;
};

struct StunProbeResult {
    StunServerConfig server;
    UdpEndpoint serverEndpoint{};
    UdpEndpoint mappedEndpoint{};
};

bool GenerateStunTransactionId(StunTransactionId* transactionId,
                               std::string* error);
std::vector<uint8_t> MakeStunBindingRequest(
    const StunTransactionId& transactionId);

// Parses an RFC 8489 Binding Success Response and extracts its
// XOR-MAPPED-ADDRESS. The response must match the supplied transaction ID.
bool ParseStunBindingResponse(const uint8_t* data, size_t len,
                              const StunTransactionId& transactionId,
                              UdpEndpoint* mappedEndpoint,
                              std::string* error);

// Sends one Binding transaction through an already-open IPv4 UDP socket.
// The caller owns the socket and must keep it open for subsequent peer punch
// traffic so the NAT mapping discovered here remains valid.
bool QueryStunBinding(socket_t sock, const StunServerConfig& server,
                      int timeoutMs, int attempts, StunProbeResult* result,
                      std::string* error);

// Probes at least two independently hosted STUN endpoints sequentially using
// exactly the same socket.
bool ProbeStunServers(socket_t sock,
                      const std::vector<StunServerConfig>& servers,
                      int timeoutMs, int attempts,
                      std::vector<StunProbeResult>* results,
                      std::string* error);

NatMappingAnalysis ClassifyNatMapping(const UdpEndpoint& first,
                                      const UdpEndpoint& second,
                                      uint16_t regularDeltaLimit = 5);
const char* NatMappingBehaviorName(NatMappingBehavior behavior);
bool ParseNatMappingBehavior(const std::string& text,
                             NatMappingBehavior* behavior);
