// Linux TUN adapter using /dev/net/tun.
// Implements the TunAdapter interface declared in tun_adapter.h.

#include "tun_adapter.h"

#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

#include "log.h"
#include "util.h"

namespace {
constexpr int kReadTimeoutMs = 500;
constexpr const char* kTunDevPath = "/dev/net/tun";
}  // namespace

class LinuxTunAdapter : public TunAdapter {
public:
	~LinuxTunAdapter() override { Close(); }

	bool Open(const Config& cfg) override {
		fd_ = ::open(kTunDevPath, O_RDWR);
		if (fd_ < 0) {
			Log(LogLevel::Error,
				std::string("Failed to open ") + kTunDevPath + ": "
				+ std::strerror(errno));
			return false;
		}

		struct ifreq ifr {};
		ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
		if (!cfg.adapter_name.empty()) {
			std::strncpy(ifr.ifr_name, cfg.adapter_name.c_str(), IFNAMSIZ - 1);
		}

		if (::ioctl(fd_, TUNSETIFF, &ifr) < 0) {
			Log(LogLevel::Error,
				std::string("TUNSETIFF failed: ") + std::strerror(errno));
			Close();
			return false;
		}

		Log(LogLevel::Info,
			std::string("TUN device opened: ") + ifr.ifr_name);

		if (cfg.auto_config_ipv4) {
			if (!ConfigureTunIpv4(cfg)) {
				Log(LogLevel::Error, "Failed to configure TUN IPv4.");
				Close();
				return false;
			}
			if (!ConfigureTunMtu(cfg)) {
				Log(LogLevel::Error, "Failed to configure TUN MTU.");
				Close();
				return false;
			}
		} else {
			Log(LogLevel::Info,
				"auto_config_ipv4=false, skip TUN IP/MTU setup.");
		}

		return true;
	}

	void Close() override {
		if (fd_ >= 0) {
			::close(fd_);
			fd_ = -1;
		}
	}

	bool ReadPacket(uint8_t* buf, size_t bufSize, size_t& outLen) override {
		outLen = 0;

		struct pollfd pfd {};
		pfd.fd = fd_;
		pfd.events = POLLIN;

		const int ret = ::poll(&pfd, 1, kReadTimeoutMs);
		if (ret < 0) {
			if (errno == EINTR) {
				return true;  // interrupted by signal, let caller check g_running
			}
			Log(LogLevel::Error,
				std::string("poll() on TUN fd failed: ") + std::strerror(errno));
			return false;
		}
		if (ret == 0) {
			return true;  // timeout, no packet
		}

		const ssize_t n = ::read(fd_, buf, bufSize);
		if (n < 0) {
			Log(LogLevel::Error,
				std::string("read() from TUN fd failed: ") + std::strerror(errno));
			return false;
		}
		outLen = static_cast<size_t>(n);
		return true;
	}

	bool WritePacket(const uint8_t* data, size_t len) override {
		const ssize_t n = ::write(fd_, data, len);
		if (n < 0) {
			Log(LogLevel::Error,
				std::string("write() to TUN fd failed: ") + std::strerror(errno));
			return false;
		}
		return true;
	}

private:
	int fd_ = -1;
};

TunAdapter* TunAdapter::Create() {
	return new LinuxTunAdapter();
}
