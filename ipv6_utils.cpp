#include "ipv6_utils.h"

#ifdef _WIN32
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cstring>
#endif

#include <algorithm>

std::vector<Ipv6AddrInfo> EnumerateLocalIpv6Addresses() {
	std::vector<Ipv6AddrInfo> result;

#ifdef _WIN32
	ULONG bufSize = 15000;
	PIP_ADAPTER_ADDRESSES addrs = nullptr;
	ULONG ret = 0;

	// Retry up to 3 times as recommended by MSDN
	for (int i = 0; i < 3; ++i) {
		addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(malloc(bufSize));
		if (!addrs) return result;

		ret = GetAdaptersAddresses(AF_INET6,
			GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
			nullptr, addrs, &bufSize);

		if (ret == ERROR_BUFFER_OVERFLOW) {
			free(addrs);
			addrs = nullptr;
			continue;
		}
		break;
	}

	if (ret != NO_ERROR || !addrs) {
		if (addrs) free(addrs);
		return result;
	}

	for (PIP_ADAPTER_ADDRESSES cur = addrs; cur; cur = cur->Next) {
		if (cur->OperStatus != IfOperStatusUp) continue;

		// Convert adapter name from wide string
		std::string ifName;
		if (cur->FriendlyName) {
			int len = WideCharToMultiByte(CP_UTF8, 0, cur->FriendlyName, -1,
				nullptr, 0, nullptr, nullptr);
			if (len > 0) {
				ifName.resize(len - 1);
				WideCharToMultiByte(CP_UTF8, 0, cur->FriendlyName, -1,
					&ifName[0], len, nullptr, nullptr);
			}
		}

		for (PIP_ADAPTER_UNICAST_ADDRESS ua = cur->FirstUnicastAddress; ua; ua = ua->Next) {
			if (ua->Address.lpSockaddr->sa_family != AF_INET6) continue;

			sockaddr_in6* sa6 = reinterpret_cast<sockaddr_in6*>(ua->Address.lpSockaddr);
			char buf[INET6_ADDRSTRLEN] = {};
			if (InetNtopA(AF_INET6, &sa6->sin6_addr, buf, sizeof(buf))) {
				Ipv6AddrInfo info;
				info.address = buf;
				info.interface_name = ifName;
				result.push_back(info);
			}
		}
	}

	free(addrs);

#else  // Linux/POSIX
	struct ifaddrs* ifap = nullptr;
	if (getifaddrs(&ifap) != 0) {
		return result;
	}

	for (struct ifaddrs* cur = ifap; cur; cur = cur->ifa_next) {
		if (!cur->ifa_addr) continue;
		if (cur->ifa_addr->sa_family != AF_INET6) continue;
		if (!(cur->ifa_flags & IFF_UP)) continue;

		sockaddr_in6* sa6 = reinterpret_cast<sockaddr_in6*>(cur->ifa_addr);
		char buf[INET6_ADDRSTRLEN] = {};
		if (inet_ntop(AF_INET6, &sa6->sin6_addr, buf, sizeof(buf))) {
			Ipv6AddrInfo info;
			info.address = buf;
			info.interface_name = cur->ifa_name ? cur->ifa_name : "";
			result.push_back(info);
		}
	}

	freeifaddrs(ifap);
#endif

	// Add "::" (any) as the first option
	result.insert(result.begin(), Ipv6AddrInfo{"::", "Any"});

	return result;
}

bool ValidateIpv6Address(const std::string& addr) {
	if (addr.empty()) return false;

#ifdef _WIN32
	in6_addr tmp{};
	return InetPtonA(AF_INET6, addr.c_str(), &tmp) == 1;
#else
	in6_addr tmp{};
	return inet_pton(AF_INET6, addr.c_str(), &tmp) == 1;
#endif
}
