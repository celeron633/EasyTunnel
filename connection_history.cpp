#include "connection_history.h"

#include <algorithm>
#include <fstream>
#include <string>

namespace {
std::string Trim(const std::string& s) {
	const char* ws = " \t\r\n";
	auto b = s.find_first_not_of(ws);
	if (b == std::string::npos) return "";
	auto e = s.find_last_not_of(ws);
	return s.substr(b, e - b + 1);
}
}  // namespace

ConnectionHistory::ConnectionHistory(const std::string& filePath)
	: filePath_(filePath) {}

void ConnectionHistory::Load() {
	peers_.clear();
	lastLocalIpv6_.clear();

	std::ifstream in(filePath_);
	if (!in.is_open()) return;

	std::string section;
	std::string line;
	while (std::getline(in, line)) {
		std::string t = Trim(line);
		if (t.empty() || t[0] == '#' || t[0] == ';') continue;

		if (t.front() == '[' && t.back() == ']') {
			section = t.substr(1, t.size() - 2);
			continue;
		}

		if (section == "history") {
			auto eq = t.find('=');
			if (eq != std::string::npos) {
				std::string key = Trim(t.substr(0, eq));
				std::string val = Trim(t.substr(eq + 1));
				if (key == "peer" && !val.empty()) {
					if (peers_.size() < kMaxHistory) {
						peers_.push_back(val);
					}
				} else if (key == "last_local_ipv6") {
					lastLocalIpv6_ = val;
				}
			}
		}
	}
}

void ConnectionHistory::Save() const {
	std::ofstream out(filePath_);
	if (!out.is_open()) return;

	out << "[history]\n";
	if (!lastLocalIpv6_.empty()) {
		out << "last_local_ipv6=" << lastLocalIpv6_ << "\n";
	}
	for (const auto& peer : peers_) {
		out << "peer=" << peer << "\n";
	}
}

void ConnectionHistory::AddPeer(const std::string& addr) {
	if (addr.empty()) return;

	// Remove if already present
	auto it = std::find(peers_.begin(), peers_.end(), addr);
	if (it != peers_.end()) {
		peers_.erase(it);
	}

	// Insert at front
	peers_.insert(peers_.begin(), addr);

	// Trim to max size
	if (peers_.size() > kMaxHistory) {
		peers_.resize(kMaxHistory);
	}
}
