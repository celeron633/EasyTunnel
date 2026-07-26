#include "config.h"

#include <filesystem>
#include <fstream>

#include <json/json.h>

namespace {
const char* ConfigLogLevel(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "Debug";
        case LogLevel::Info: return "Info";
        case LogLevel::Warn: return "Warn";
        case LogLevel::Error: return "Error";
        default: return "Info";
    }
}

// A missing key or a key whose JSON type does not match keeps the struct's
// current value instead of failing the whole file.
void ReadString(const Json::Value& root, const char* key, std::string* value) {
    if (root.isMember(key) && root[key].isString()) *value = root[key].asString();
}

void ReadUInt16(const Json::Value& root, const char* key, uint16_t* value) {
    if (root.isMember(key) && root[key].isUInt() && root[key].asUInt() <= 65535) {
        *value = static_cast<uint16_t>(root[key].asUInt());
    }
}

void ReadBool(const Json::Value& root, const char* key, bool* value) {
    if (root.isMember(key) && root[key].isBool()) *value = root[key].asBool();
}

bool WriteConfig(const std::string& path, const RendezvousConfig& config,
                 std::string* error) {
    Json::Value root;
    root["bind_address"] = config.bindAddress;
    root["port"] = config.port;
    root["auth_token"] = config.authToken;
    root["client_timeout_seconds"] = config.clientTimeoutSeconds;
    root["max_clients_per_room"] = config.maxClientsPerRoom;
    root["ipv4_relay_enabled"] = config.ipv4RelayEnabled;
    root["ipv4_relay_port_start"] = config.ipv4RelayPortStart;
    root["ipv4_relay_port_end"] = config.ipv4RelayPortEnd;
    root["log_level"] = ConfigLogLevel(config.logLevel);
    root["log_file"] = config.logFile;

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        *error = "Cannot create rendezvous config: " + path;
        return false;
    }
    Json::StreamWriterBuilder writerBuilder;
    writerBuilder["indentation"] = "  ";
    output << Json::writeString(writerBuilder, root) << '\n';
    if (!output.good()) {
        *error = "Cannot write rendezvous config: " + path;
        return false;
    }
    return true;
}
}  // namespace

bool LoadOrCreateRendezvousConfig(const std::string& path, RendezvousConfig* config,
                                  bool* created, std::string* error) {
    *created = false;
    std::error_code fsError;
    if (!std::filesystem::exists(path, fsError)) {
        if (!WriteConfig(path, *config, error)) return false;
        *created = true;
        return true;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        *error = "Cannot open rendezvous config: " + path;
        return false;
    }

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string parseErrors;
    if (!Json::parseFromStream(builder, input, &root, &parseErrors)
        || !root.isObject()) {
        *error = "Invalid JSON object in " + path;
        return false;
    }

    ReadString(root, "bind_address", &config->bindAddress);
    ReadUInt16(root, "port", &config->port);
    ReadString(root, "auth_token", &config->authToken);
    ReadUInt16(root, "client_timeout_seconds", &config->clientTimeoutSeconds);
    ReadUInt16(root, "max_clients_per_room", &config->maxClientsPerRoom);
    ReadBool(root, "ipv4_relay_enabled", &config->ipv4RelayEnabled);
    ReadUInt16(root, "ipv4_relay_port_start", &config->ipv4RelayPortStart);
    ReadUInt16(root, "ipv4_relay_port_end", &config->ipv4RelayPortEnd);
    if (root.isMember("log_level") && root["log_level"].isString()
        && !TryParseLogLevel(root["log_level"].asString(), &config->logLevel)) {
        *error = "Invalid log_level in " + path + ": " + root["log_level"].asString();
        return false;
    }
    ReadString(root, "log_file", &config->logFile);

    return ValidateRendezvousConfig(*config, error);
}

bool ValidateRendezvousConfig(const RendezvousConfig& config, std::string* error) {
    if (config.bindAddress.empty()) {
        *error = "bind_address cannot be empty";
        return false;
    }
    if (config.port == 0) {
        *error = "port must be 1..65535";
        return false;
    }
    if (config.clientTimeoutSeconds < 5 || config.clientTimeoutSeconds > 3600) {
        *error = "client_timeout_seconds must be 5..3600";
        return false;
    }
    if (config.maxClientsPerRoom < 2 || config.maxClientsPerRoom > 32) {
        *error = "max_clients_per_room must be 2..32";
        return false;
    }
    if (config.ipv4RelayPortStart == 0 || config.ipv4RelayPortEnd == 0
        || config.ipv4RelayPortStart > config.ipv4RelayPortEnd) {
        *error = "ipv4_relay_port_start/end must define a valid UDP port range";
        return false;
    }
    return true;
}

bool SaveRendezvousConfig(const std::string& path, const RendezvousConfig& config,
                          std::string* error) {
    if (!ValidateRendezvousConfig(config, error)) return false;
    return WriteConfig(path, config, error);
}
