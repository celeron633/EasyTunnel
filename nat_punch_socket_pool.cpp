#include "nat_punch_socket_pool.h"

#include <cerrno>

#ifndef _WIN32
#include <poll.h>
#endif

namespace {
socket_t OpenBoundIpv4UdpSocket(int receiveTimeoutMs, int* socketError) {
    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSocket) {
        if (socketError != nullptr) *socketError = GetSocketError();
        return sock;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    if (bind(sock, reinterpret_cast<const sockaddr*>(&local),
             static_cast<socket_len_t>(sizeof(local))) != 0) {
        if (socketError != nullptr) *socketError = GetSocketError();
        CloseSocket(sock);
        return kInvalidSocket;
    }
    SetSocketRecvTimeoutMs(sock, receiveTimeoutMs);
    return sock;
}

UdpEndpoint FromSockaddr(const sockaddr_storage& address, socket_len_t len) {
    UdpEndpoint endpoint{};
    endpoint.addr = address;
    endpoint.addr_len = len;
    endpoint.family = address.ss_family;
    return endpoint;
}

bool ReceiveDatagram(socket_t sock, std::vector<uint8_t>* buffer,
                     UdpEndpoint* source, int* received,
                     std::string* error) {
    sockaddr_storage sourceAddress{};
    socket_len_t sourceLen = static_cast<socket_len_t>(sizeof(sourceAddress));
    *received = recvfrom(sock, reinterpret_cast<char*>(buffer->data()),
        static_cast<int>(buffer->size()), 0,
        reinterpret_cast<sockaddr*>(&sourceAddress), &sourceLen);
    if (*received >= 0) {
        *source = FromSockaddr(sourceAddress, sourceLen);
        return true;
    }
    const int socketError = GetSocketError();
    if (IsRecvTimeout(socketError)
        || IsUdpDestinationUnreachable(socketError)) {
        return true;
    }
    *error = "UDP receive failed during adaptive NAT traversal. err="
        + std::to_string(socketError);
    return false;
}
}  // namespace

NatPunchSocketPool::NatPunchSocketPool(
        socket_t primary, int receiveTimeoutMs,
        NatPunchSocketOpener opener)
    : receiveTimeoutMs_(receiveTimeoutMs),
      opener_(opener == nullptr ? OpenBoundIpv4UdpSocket : opener) {
    if (primary != kInvalidSocket) sockets_.push_back(primary);
}

NatPunchSocketPool::~NatPunchSocketPool() {
    CloseAll();
}

bool NatPunchSocketPool::GrowTo(size_t desiredCount, int* socketError) {
    if (socketError != nullptr) *socketError = 0;
    while (sockets_.size() < desiredCount) {
        socket_t auxiliary = opener_(receiveTimeoutMs_, socketError);
        if (auxiliary == kInvalidSocket) return false;
        sockets_.push_back(auxiliary);
    }
    return true;
}

bool NatPunchSocketPool::Receive(
        int timeoutMs, std::vector<uint8_t>* buffer,
        socket_t* receivingSocket, UdpEndpoint* source,
        int* received, std::string* error) const {
    *receivingSocket = kInvalidSocket;
    *received = -1;
    if (sockets_.empty()) {
        *error = "No NAT punch sockets are available";
        return false;
    }
#ifdef _WIN32
    std::vector<WSAPOLLFD> descriptors;
    descriptors.reserve(sockets_.size());
    for (const socket_t sock : sockets_) {
        WSAPOLLFD descriptor{};
        descriptor.fd = sock;
        descriptor.events = POLLRDNORM;
        descriptors.push_back(descriptor);
    }
    const int ready = WSAPoll(descriptors.data(),
        static_cast<ULONG>(descriptors.size()), timeoutMs);
    if (ready == SOCKET_ERROR) {
        const int socketError = GetSocketError();
        if (socketError == WSAEINTR) return true;
        *error = "UDP poll failed during adaptive NAT traversal. err="
            + std::to_string(socketError);
        return false;
    }
    if (ready == 0) return true;
    for (size_t i = 0; i < descriptors.size(); ++i) {
        if ((descriptors[i].revents
             & (POLLRDNORM | POLLERR | POLLHUP)) == 0) {
            continue;
        }
        *receivingSocket = sockets_[i];
        return ReceiveDatagram(
            sockets_[i], buffer, source, received, error);
    }
#else
    std::vector<pollfd> descriptors;
    descriptors.reserve(sockets_.size());
    for (const socket_t sock : sockets_) {
        pollfd descriptor{};
        descriptor.fd = sock;
        descriptor.events = POLLIN;
        descriptors.push_back(descriptor);
    }
    const int ready = poll(descriptors.data(), descriptors.size(), timeoutMs);
    if (ready < 0) {
        if (errno == EINTR) return true;
        *error = "UDP poll failed during adaptive NAT traversal. err="
            + std::to_string(errno);
        return false;
    }
    if (ready == 0) return true;
    for (size_t i = 0; i < descriptors.size(); ++i) {
        if ((descriptors[i].revents & (POLLIN | POLLERR | POLLHUP)) == 0) {
            continue;
        }
        *receivingSocket = sockets_[i];
        return ReceiveDatagram(
            sockets_[i], buffer, source, received, error);
    }
#endif
    return true;
}

socket_t NatPunchSocketPool::ReleaseWinner(socket_t winner) {
    bool found = false;
    for (socket_t& sock : sockets_) {
        if (sock == winner) {
            found = true;
        } else {
            CloseSocket(sock);
        }
    }
    sockets_.clear();
    return found ? winner : kInvalidSocket;
}

void NatPunchSocketPool::CloseAll() {
    for (socket_t& sock : sockets_) CloseSocket(sock);
    sockets_.clear();
}
