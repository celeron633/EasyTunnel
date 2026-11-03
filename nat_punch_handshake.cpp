#include "nat_punch_handshake.h"

#include <utility>

NatPunchHandshake::NatPunchHandshake(
        NatPunchExecutionRole role, std::string nonce)
    : role_(role), nonce_(std::move(nonce)) {}

bool NatPunchHandshake::awaitingConfirmation() const {
    return role_ == NatPunchExecutionRole::Receiver
        && socket_ != kInvalidSocket && !complete_;
}

NatPunchReply NatPunchHandshake::Handle(
        socket_t socket, const UdpEndpoint& source,
        const std::string& type, const std::string& nonce) {
    if (socket == kInvalidSocket) return NatPunchReply::None;
    if (socket_ != kInvalidSocket
        && (socket != socket_ || !SameUdpEndpoint(source, peer_))) {
        return NatPunchReply::None;
    }
    if (role_ == NatPunchExecutionRole::Sender) {
        if (type != "PUNCH_ACK" || nonce != nonce_) return NatPunchReply::None;
        socket_ = socket;
        peer_ = source;
        complete_ = true;
        return NatPunchReply::Confirm;
    }
    if (type == "PUNCH") {
        if (socket_ == kInvalidSocket) {
            socket_ = socket;
            peer_ = source;
            nonce_ = nonce;
        }
        return nonce == nonce_ ? NatPunchReply::Ack : NatPunchReply::None;
    }
    if (type == "PUNCH_CONFIRM" && socket_ != kInvalidSocket && nonce == nonce_) {
        complete_ = true;
    }
    return NatPunchReply::None;
}
