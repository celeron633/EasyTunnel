// Windows TUN adapter using Wintun.
// Implements the TunAdapter interface declared in tun_adapter.h.

#include "tun_adapter.h"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstring>
#include <string>

#include "log.h"
#include "util.h"
#include "wintun_loader.h"

namespace {
constexpr DWORD kWintunRingCapacity = 0x400000;
constexpr DWORD kReadWaitMs = 500;
}  // namespace

class WintunAdapter : public TunAdapter {
public:
	~WintunAdapter() override { Close(); }

	bool Open(const Config& cfg) override {
		if (!LoadWintunLibrary(L"wintun.dll")) {
			Log(LogLevel::Error,
				std::string("Failed to load Wintun: ") + GetWintunLoadError());
			return false;
		}

		const std::wstring name = Utf8ToWide(cfg.adapter_name);
		const std::wstring type = Utf8ToWide(cfg.tunnel_type);

		adapter_ = WtOpenAdapter(name.c_str());
		if (adapter_ == nullptr) {
			Log(LogLevel::Warn, "Adapter not found, creating new adapter...");
			adapter_ = WtCreateAdapter(name.c_str(), type.c_str(), nullptr);
			if (adapter_ == nullptr) {
				Log(LogLevel::Error,
					"WintunCreateAdapter failed. last_error="
					+ std::to_string(GetLastError()));
				return false;
			}
		} else {
			Log(LogLevel::Info, "Opened existing adapter.");
		}

		if (cfg.auto_config_ipv4) {
			if (!ConfigureTunIpv4(cfg)) {
				Log(LogLevel::Error,
					"Failed to set adapter IPv4. Run as administrator.");
				return false;
			}
			if (!ConfigureTunMtu(cfg)) {
				Log(LogLevel::Error,
					"Failed to set adapter MTU. Run as administrator.");
				return false;
			}
			if (!DisableTunIpv6(cfg)) {
				Log(LogLevel::Error,
					"Failed to disable adapter IPv6 binding. Run as administrator.");
				return false;
			}
		} else {
			Log(LogLevel::Info,
				"auto_config_ipv4=false, skip adapter IPv4/MTU/IPv6 setup.");
		}

		session_ = WtStartSession(adapter_, kWintunRingCapacity);
		if (session_ == nullptr) {
			Log(LogLevel::Error,
				"WintunStartSession failed. last_error="
				+ std::to_string(GetLastError()));
			return false;
		}

		readEvent_ = WtGetReadWaitEvent(session_);
		return true;
	}

	void Close() override {
		if (session_ != nullptr) {
			WtEndSession(session_);
			session_ = nullptr;
		}
		if (adapter_ != nullptr) {
			WtCloseAdapter(adapter_);
			adapter_ = nullptr;
		}
		UnloadWintunLibrary();
		readEvent_ = nullptr;
	}

	bool ReadPacket(uint8_t* buf, size_t bufSize, size_t& outLen) override {
		outLen = 0;
		DWORD pktSize = 0;
		BYTE* pkt = WtReceivePacket(session_, &pktSize);
		if (pkt == nullptr) {
			const DWORD err = GetLastError();
			if (err == ERROR_NO_MORE_ITEMS) {
				WaitForSingleObject(readEvent_, kReadWaitMs);
				return true;  // timeout – outLen stays 0
			}
			if (err == ERROR_HANDLE_EOF) {
				Log(LogLevel::Warn, "Wintun session EOF");
				return false;
			}
			Log(LogLevel::Error,
				"WintunReceivePacket failed. err=" + std::to_string(err));
			return false;  // unexpected receive error is fatal
		}

		if (pktSize > static_cast<DWORD>(bufSize)) {
			Log(LogLevel::Warn,
				"Wintun packet too large for buffer, drop. size="
				+ std::to_string(pktSize));
			WtReleaseReceivePacket(session_, pkt);
			return true;
		}

		std::memcpy(buf, pkt, pktSize);
		outLen = pktSize;
		WtReleaseReceivePacket(session_, pkt);
		return true;
	}

	bool WritePacket(const uint8_t* data, size_t len) override {
		BYTE* out = WtAllocateSendPacket(session_, static_cast<DWORD>(len));
		if (out == nullptr) {
			Log(LogLevel::Warn,
				"WintunAllocateSendPacket failed, drop packet. err="
				+ std::to_string(GetLastError()));
			return false;
		}
		std::memcpy(out, data, len);
		WtSendPacket(session_, out);
		return true;
	}

private:
	WINTUN_ADAPTER_HANDLE adapter_ = nullptr;
	WINTUN_SESSION_HANDLE session_ = nullptr;
	HANDLE readEvent_ = nullptr;
};

TunAdapter* TunAdapter::Create() {
	return new WintunAdapter();
}
