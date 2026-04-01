#pragma once

#include <cstdint>
#include <string>

#include "config.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

bool IsIpv4Packet(const uint8_t* data, size_t len);
std::wstring Utf8ToWide(const std::string& s);
std::string PrefixToMask(uint8_t prefix);
bool RunCommand(const std::string& cmd);
bool ConfigureTunIpv4(const Config& cfg);
bool ConfigureTunMtu(const Config& cfg);
bool ParseIpv6(const std::string& ip, in6_addr* out);
