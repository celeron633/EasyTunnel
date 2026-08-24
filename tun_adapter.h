#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"

enum class TunReadResult {
	Packet,
	NoPacket,
	Closed,
	Error
};

enum class TunWriteResult {
	Written,
	RingFull,
	Closed,
	Error
};

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
	// Packet means buf/outLen contain a packet; NoPacket is a soft timeout or
	// interrupt; Closed and Error are terminal and deliberately distinct.
	virtual TunReadResult ReadPacket(
		uint8_t* buf, size_t bufSize, size_t& outLen) = 0;

	// Write one packet to the TUN device.
	// RingFull is a non-fatal packet drop. Closed and Error are terminal.
	virtual TunWriteResult WritePacket(const uint8_t* data, size_t len) = 0;

	// Factory – returns the platform-appropriate implementation.
	static TunAdapter* Create();
};
