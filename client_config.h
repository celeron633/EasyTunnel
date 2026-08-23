#pragma once

#include <string>
#include <vector>

#include "config.h"

// Persistent settings shared by the console, GUI and TUI clients. All three
// read and write the same JSON file, so switching front-ends keeps the
// rendezvous account, TUN parameters and traversal strategy intact.
inline constexpr const char* kClientConfigFileName = "EasyTunnel.json";

struct ClientConfig {
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
    int nat4RoundLimit = 3;
    std::vector<TraversalModeSetting> traversalModes = DefaultTraversalModes();
    bool ipv6AcceptInbound = false;
    int ipv6ListenPort = 0;
    std::string ipv6ProbeHost = "2400:3200::1";
    int ipv6ProbePort = 53;
    int ipv6FallbackTimeout = 15;
    int logLevel = 1;
    int rendezvousRetryDelaySeconds = 5;
    bool autoWaitForPeer = false;
    bool closeToMinimize = false;
};

// Loads the JSON file. A missing file is not an error: *existed is set to
// false and the config keeps its defaults. Out-of-range numbers are clamped.
bool LoadClientConfig(const std::string& path, ClientConfig* config,
                      bool* existed, std::string* error);
bool SaveClientConfig(const std::string& path, const ClientConfig& config,
                      std::string* error);

// Checks the fields no clamp can fix: empty or unsafe identity strings, an
// unparsable TUN address, or an unusable traversal strategy.
bool ValidateClientConfig(const ClientConfig& config, std::string* error);

// Maps the persistent settings onto the tunnel engine's runtime configuration.
Config ToEngineConfig(const ClientConfig& config,
                      const std::string& targetPeerId);
