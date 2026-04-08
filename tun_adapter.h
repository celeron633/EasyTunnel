#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"

// Platform-agnostic TUN adapter interface.
// Concrete implementations:
//   tun_adapter_windows.cpp  – Windows, uses Wintun driver.
//   tun_adapter_linux.cpp    – Linux, uses /dev/net/tun.
class TunAdapter {
public:
	virtual ~TunAdapter() = default;

	// Open / create the TUN device and, when cfg.auto_config_ipv4 is true,
	// configure the IPv4 address and MTU.  Must be called before
	// ReadPacket / WritePacket.
	// Returns true on success, false on failure (error already logged).
	virtual bool Open(const Config& cfg) = 0;

	// Close the TUN device and release all resources.
	virtual void Close() = 0;

	// Read one packet from the TUN device.
	// Blocks for up to ~500 ms waiting for a packet.
	// Returns true with outLen > 0  : a packet was placed in buf.
	// Returns true with outLen == 0 : timeout, no packet yet (caller should
	//                                 check g_running and retry).
	// Returns false                 : fatal error or session closed.
	virtual bool ReadPacket(uint8_t* buf, size_t bufSize, size_t& outLen) = 0;

	// Write one packet to the TUN device.
	// Returns true on success, false on error (error already logged).
	virtual bool WritePacket(const uint8_t* data, size_t len) = 0;

	// Factory – returns the platform-appropriate implementation.
	static TunAdapter* Create();
};
