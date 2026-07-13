#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "../util.h"
#include "config.h"

class RendezvousRegistry {
public:
    RendezvousRegistry(socket_t sock, const RendezvousConfig& config);
    ~RendezvousRegistry();

    RendezvousRegistry(const RendezvousRegistry&) = delete;
    RendezvousRegistry& operator=(const RendezvousRegistry&) = delete;

    void Handle(const UdpEndpoint& source, const std::string& type,
                const std::vector<std::string>& fields,
                std::chrono::steady_clock::time_point now);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
