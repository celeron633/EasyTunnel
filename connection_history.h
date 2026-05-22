#pragma once

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// ConnectionHistory: manages persistent history of peer IPv6 addresses
// stored in a .ini file for quick re-selection.
// ---------------------------------------------------------------------------

class ConnectionHistory {
public:
	explicit ConnectionHistory(const std::string& filePath = "6Tunnel.ini");

	// Load history from file
	void Load();

	// Save history to file
	void Save() const;

	// Add a peer address to history (moves to front if already present)
	void AddPeer(const std::string& addr);

	// Get list of previously connected peer addresses
	const std::vector<std::string>& GetPeers() const { return peers_; }

	// Get/set last used local IPv6
	const std::string& GetLastLocalIpv6() const { return lastLocalIpv6_; }
	void SetLastLocalIpv6(const std::string& addr) { lastLocalIpv6_ = addr; }

private:
	std::string filePath_;
	std::vector<std::string> peers_;
	std::string lastLocalIpv6_;
	static constexpr size_t kMaxHistory = 20;
};
