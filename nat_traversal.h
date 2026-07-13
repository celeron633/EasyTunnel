#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include "config.h"
#include "util.h"

constexpr size_t kPeerDummyTrafficPacketSize = 1024;

bool OpenNatUdpSocket(const Config& cfg, int recvTimeoutMs, socket_t* sock,
                      UdpEndpoint* server, std::string* error);

bool DiscoverAndPunch(socket_t* sock, const Config& cfg,
                      const UdpEndpoint& server, const std::atomic<bool>& running,
                      UdpEndpoint* peer, std::string* error);

bool HandlePeerControl(socket_t sock, const Config& cfg,
                       const UdpEndpoint& peer, const UdpEndpoint& source,
                       const uint8_t* data, size_t len,
                       std::chrono::steady_clock::time_point* lastPeerSeen,
                       bool* consumedDummyTraffic = nullptr);

bool SendPeerKeepalive(socket_t sock, const Config& cfg, const UdpEndpoint& peer);
bool SendPeerDummyTraffic(socket_t sock, const Config& cfg, const UdpEndpoint& peer);
void UnregisterRendezvous(socket_t sock, const Config& cfg, const UdpEndpoint& server);

bool ListRendezvousClients(const std::string& serverAddress, uint16_t serverPort,
                           const std::string& roomId, const std::string& authToken,
                           std::vector<std::string>* clients, std::string* error);
