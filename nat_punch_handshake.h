#pragma once

#include <string>

#include "nat_punch_plan.h"

enum class NatPunchReply { None, Ack, Confirm };

// Only feed packets after session, attempt, peer, token and source-IP validation.
// Once selected, a path never changes within this attempt.
class NatPunchHandshake {
public:
    NatPunchHandshake(NatPunchExecutionRole role, std::string nonce);
    NatPunchReply Handle(socket_t socket, const UdpEndpoint& source,
                         const std::string& type, const std::string& nonce);
    bool complete() const { return complete_; }
    bool awaitingConfirmation() const;
    socket_t socket() const { return socket_; }
    const UdpEndpoint& peer() const { return peer_; }
    const std::string& nonce() const { return nonce_; }

private:
    NatPunchExecutionRole role_;
    std::string nonce_;
    socket_t socket_ = kInvalidSocket;
    UdpEndpoint peer_{};
    bool complete_ = false;
};
