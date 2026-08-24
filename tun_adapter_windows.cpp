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
#include <netioapi.h>

#include <cstring>
#include <string>

#include "log.h"
#include "util.h"
#include "wintun_loader.h"

namespace {
constexpr DWORD kWintunRingCapacity = 0x400000;
constexpr DWORD kReadWaitMs = 500;

std::string FormatNetworkError(NETIO_STATUS status) {
	char* message = nullptr;
	DWORD length = FormatMessageA(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, status, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		reinterpret_cast<char*>(&message), 0, nullptr);

	std::string result = "error=" + std::to_string(status);
	if (length != 0 && message != nullptr) {
		while (length > 0 &&
			   (message[length - 1] == '\r' || message[length - 1] == '\n')) {
			message[--length] = '\0';
		}
		result += " (";
		result += message;
		result += ')';
	}
	if (message != nullptr) {
		LocalFree(message);
	}
	return result;
}

bool ConfigureTunIpv4(const Config& cfg, const NET_LUID& adapterLuid) {
	in_addr address{};
	if (!ParseIpv4(cfg.local_tun_ipv4, &address)) {
		Log(LogLevel::Error, "Invalid TUN IPv4 address: " + cfg.local_tun_ipv4);
		return false;
	}

	bool addressExists = false;
	MIB_UNICASTIPADDRESS_TABLE* table = nullptr;
	NETIO_STATUS status = GetUnicastIpAddressTable(AF_INET, &table);
	if (status != NO_ERROR) {
		Log(LogLevel::Error,
			"GetUnicastIpAddressTable failed: " + FormatNetworkError(status));
		return false;
	}

	for (ULONG i = 0; i < table->NumEntries; ++i) {
		MIB_UNICASTIPADDRESS_ROW& existing = table->Table[i];
		if (existing.InterfaceLuid.Value != adapterLuid.Value) {
			continue;
		}

		const bool isDesired =
			existing.Address.si_family == AF_INET &&
			existing.Address.Ipv4.sin_addr.S_un.S_addr == address.S_un.S_addr &&
			existing.OnLinkPrefixLength == cfg.tun_prefix;
		if (isDesired) {
			addressExists = true;
			continue;
		}

		status = DeleteUnicastIpAddressEntry(&existing);
		if (status != NO_ERROR && status != ERROR_NOT_FOUND) {
			Log(LogLevel::Error,
				"DeleteUnicastIpAddressEntry failed: " + FormatNetworkError(status));
			FreeMibTable(table);
			return false;
		}
	}
	FreeMibTable(table);

	if (!addressExists) {
		MIB_UNICASTIPADDRESS_ROW row{};
		InitializeUnicastIpAddressEntry(&row);
		row.InterfaceLuid = adapterLuid;
		row.Address.Ipv4.sin_family = AF_INET;
		row.Address.Ipv4.sin_addr = address;
		row.OnLinkPrefixLength = cfg.tun_prefix;
		row.PrefixOrigin = IpPrefixOriginManual;
		row.SuffixOrigin = IpSuffixOriginManual;
		row.ValidLifetime = 0xFFFFFFFF;
		row.PreferredLifetime = 0xFFFFFFFF;

		status = CreateUnicastIpAddressEntry(&row);
		if (status != NO_ERROR && status != ERROR_OBJECT_ALREADY_EXISTS) {
			Log(LogLevel::Error,
				"CreateUnicastIpAddressEntry failed: " + FormatNetworkError(status));
			return false;
		}
	}

	Log(LogLevel::Info,
		"Configured adapter IPv4 " + cfg.local_tun_ipv4 + "/" +
			std::to_string(cfg.tun_prefix) + " via IP Helper API.");
	return true;
}

bool ConfigureTunMtu(const Config& cfg, const NET_LUID& adapterLuid) {
	MIB_IPINTERFACE_ROW row{};
	InitializeIpInterfaceEntry(&row);
	row.Family = AF_INET;
	row.InterfaceLuid = adapterLuid;

	NETIO_STATUS status = GetIpInterfaceEntry(&row);
	if (status != NO_ERROR) {
		Log(LogLevel::Error,
			"GetIpInterfaceEntry failed: " + FormatNetworkError(status));
		return false;
	}

	row.NlMtu = cfg.tun_mtu;
	status = SetIpInterfaceEntry(&row);
	if (status != NO_ERROR) {
		Log(LogLevel::Error,
			"SetIpInterfaceEntry failed: " + FormatNetworkError(status));
		return false;
	}

	Log(LogLevel::Info,
		"Configured adapter IPv4 MTU " + std::to_string(cfg.tun_mtu) +
			" via IP Helper API.");
	return true;
}
}  // namespace

class WintunAdapter : public TunAdapter {
public:
	~WintunAdapter() override { Close(); }

	bool Open(const Config& cfg) override {
		if (!LoadWintunLibrary()) {
			Log(LogLevel::Error,
				std::string("Failed to load Wintun: ") + GetWintunLoadError());
			return false;
		}
		Log(LogLevel::Info,
			std::string("Loaded Wintun DLL from ") + GetWintunLibraryPath());

		const std::wstring name = Utf8ToWide(cfg.adapter_name);
		const std::wstring type = L"EasyTunnel";

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

		NET_LUID adapterLuid{};
		WtGetAdapterLuid(adapter_, &adapterLuid);

		if (cfg.auto_config_ipv4) {
			if (!ConfigureTunIpv4(cfg, adapterLuid)) {
				Log(LogLevel::Error,
					"Failed to set adapter IPv4. Run as administrator.");
				return false;
			}
			if (!ConfigureTunMtu(cfg, adapterLuid)) {
				Log(LogLevel::Error,
					"Failed to set adapter MTU. Run as administrator.");
				return false;
			}
		} else {
			Log(LogLevel::Info,
				"auto_config_ipv4=false, skip adapter IPv4/MTU setup.");
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

	TunReadResult ReadPacket(
		uint8_t* buf, size_t bufSize, size_t& outLen) override {
		outLen = 0;
		if (session_ == nullptr) {
			return TunReadResult::Closed;
		}

		DWORD pktSize = 0;
		BYTE* pkt = WtReceivePacket(session_, &pktSize);
		if (pkt == nullptr) {
			const DWORD err = GetLastError();
			if (err == ERROR_NO_MORE_ITEMS) {
				const DWORD waitResult = WaitForSingleObject(readEvent_, kReadWaitMs);
				if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_TIMEOUT) {
					return TunReadResult::NoPacket;
				}
				Log(LogLevel::Error,
					"WaitForSingleObject on Wintun failed. err="
					+ std::to_string(GetLastError()));
				return TunReadResult::Error;
			}
			if (err == ERROR_HANDLE_EOF) {
				return TunReadResult::Closed;
			}
			Log(LogLevel::Error,
				"WintunReceivePacket failed. err=" + std::to_string(err));
			return TunReadResult::Error;
		}

		if (pktSize > static_cast<DWORD>(bufSize)) {
			Log(LogLevel::Warn,
				"Wintun packet too large for buffer, drop. size="
				+ std::to_string(pktSize));
			WtReleaseReceivePacket(session_, pkt);
			return TunReadResult::NoPacket;
		}

		std::memcpy(buf, pkt, pktSize);
		outLen = pktSize;
		WtReleaseReceivePacket(session_, pkt);
		return TunReadResult::Packet;
	}

	TunWriteResult WritePacket(const uint8_t* data, size_t len) override {
		if (session_ == nullptr) {
			return TunWriteResult::Closed;
		}
		if (len > WINTUN_MAX_IP_PACKET_SIZE) {
			Log(LogLevel::Error,
				"Packet exceeds Wintun maximum IP packet size: "
				+ std::to_string(len));
			return TunWriteResult::Error;
		}

		BYTE* out = WtAllocateSendPacket(session_, static_cast<DWORD>(len));
		if (out == nullptr) {
			const DWORD err = GetLastError();
			if (err == ERROR_BUFFER_OVERFLOW) {
				return TunWriteResult::RingFull;
			}
			if (err == ERROR_HANDLE_EOF) {
				return TunWriteResult::Closed;
			}
			Log(LogLevel::Error,
				"WintunAllocateSendPacket failed. err=" + std::to_string(err));
			return TunWriteResult::Error;
		}
		std::memcpy(out, data, len);
		WtSendPacket(session_, out);
		return TunWriteResult::Written;
	}

private:
	WINTUN_ADAPTER_HANDLE adapter_ = nullptr;
	WINTUN_SESSION_HANDLE session_ = nullptr;
	HANDLE readEvent_ = nullptr;
};

TunAdapter* TunAdapter::Create() {
	return new WintunAdapter();
}
