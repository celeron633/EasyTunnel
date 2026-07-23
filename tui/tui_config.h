#pragma once

#include <string>

struct TuiConfig {
    std::string rendezvousAddress = "127.0.0.1";
    int rendezvousPort = 3478;
    std::string roomId = "default-room";
    std::string peerId = "node-a";
    std::string authToken;
    std::string adapterName = "EasyTunnel";
    std::string localTunIpv4 = "10.66.0.1";
    int tunPrefix = 24;
    int tunMtu = 1452;
    bool autoConfigIpv4 = true;
    int keepaliveInterval = 15;
    int peerTimeout = 45;
    bool dummyTrafficEnabled = false;
    int punchTimeout = 30;
    int nat4SourcePortStart = 30000;
    int nat4SourcePortCount = 25;
    int nat4PeerPortOffset = 20;
    int nat4RoundTimeout = 10;
    bool ipv6FallbackEnabled = false;
    bool ipv6AcceptInbound = false;
    int ipv6ListenPort = 0;
    std::string ipv6ProbeHost = "2400:3200::1";
    int ipv6ProbePort = 53;
    int ipv6FallbackTimeout = 15;
    bool ipv4RelayFallbackEnabled = false;
    int logLevel = 1;
    int rendezvousRetryDelaySeconds = 5;
    bool autoWaitForPeer = false;
};

bool LoadTuiConfig(const std::string& path, TuiConfig* config,
                   bool* existed, std::string* error);
bool SaveTuiConfig(const std::string& path, const TuiConfig& config,
                   std::string* error);
