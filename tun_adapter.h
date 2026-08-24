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

class TunAdapter;

// A packet borrowed from a TUN adapter. The backing memory remains valid until
// this object is destroyed. Destruction returns Wintun ring storage to the
// driver; adapters with their own read buffer use a no-op release. The adapter
// must outlive every packet it issued.
class TunReadPacket {
public:
	TunReadPacket() = default;
	~TunReadPacket();

	TunReadPacket(const TunReadPacket&) = delete;
	TunReadPacket& operator=(const TunReadPacket&) = delete;

	const uint8_t* Data() const { return data_; }
	size_t Size() const { return size_; }

private:
	friend class TunAdapter;
	void Reset();

	TunAdapter* owner_ = nullptr;
	const uint8_t* data_ = nullptr;
	size_t size_ = 0;
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
	// Packet means packet borrows a valid adapter buffer; NoPacket is a soft
	// timeout or interrupt; Closed and Error are terminal. The caller must keep
	// packet alive until it has finished consuming the data.
	virtual TunReadResult ReadPacket(TunReadPacket& packet) = 0;

	// Write one packet to the TUN device.
	// RingFull is a non-fatal packet drop. Closed and Error are terminal.
	virtual TunWriteResult WritePacket(const uint8_t* data, size_t len) = 0;

	// Factory – returns the platform-appropriate implementation.
	static TunAdapter* Create();

protected:
	void SetReadPacket(TunReadPacket& packet, const uint8_t* data, size_t size) {
		packet.Reset();
		packet.owner_ = this;
		packet.data_ = data;
		packet.size_ = size;
	}

	void ResetReadPacket(TunReadPacket& packet) { packet.Reset(); }
	virtual void ReleaseReadPacket(const uint8_t* data) = 0;

private:
	friend class TunReadPacket;
};

inline TunReadPacket::~TunReadPacket() {
	Reset();
}

inline void TunReadPacket::Reset() {
	TunAdapter* owner = owner_;
	const uint8_t* data = data_;
	owner_ = nullptr;
	data_ = nullptr;
	size_ = 0;
	if (owner != nullptr) {
		owner->ReleaseReadPacket(data);
	}
}
