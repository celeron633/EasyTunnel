#pragma once

#include <string>
#include <vector>

struct Ipv6AddrInfo {
	std::string address;
	std::string interface_name;
};

// Enumerate all IPv6 addresses on the local machine.
// Returns a list of (address, interface_name) pairs.
std::vector<Ipv6AddrInfo> EnumerateLocalIpv6Addresses();

// Validate an IPv6 address string. Returns true if valid.
bool ValidateIpv6Address(const std::string& addr);
