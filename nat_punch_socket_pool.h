#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "util.h"

using NatPunchSocketOpener = socket_t (*)(int receiveTimeoutMs,
                                           int* socketError);

// Owns every socket created for one NAT punch attempt. Releasing the winning
// socket transfers only that socket to the tunnel data plane; every other exit
// closes the complete pool automatically.
class NatPunchSocketPool {
public:
    explicit NatPunchSocketPool(
        socket_t primary, int receiveTimeoutMs,
        NatPunchSocketOpener opener = nullptr);
    ~NatPunchSocketPool();

    NatPunchSocketPool(const NatPunchSocketPool&) = delete;
    NatPunchSocketPool& operator=(const NatPunchSocketPool&) = delete;

    bool GrowTo(size_t desiredCount, int* socketError);
    bool Receive(int timeoutMs, std::vector<uint8_t>* buffer,
                 socket_t* receivingSocket, UdpEndpoint* source,
                 int* received, std::string* error) const;
    socket_t ReleaseWinner(socket_t winner);
    void CloseAll();

    size_t size() const { return sockets_.size(); }
    const std::vector<socket_t>& sockets() const { return sockets_; }

private:
    int receiveTimeoutMs_;
    NatPunchSocketOpener opener_;
    std::vector<socket_t> sockets_;
};
