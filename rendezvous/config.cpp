#include "config.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
std::string EscapeJson(const std::string& value) {
    std::string output;
    output.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '\\': output += "\\\\"; break;
            case '"': output += "\\\""; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default: output += c; break;
        }
    }
    return output;
}

const char* ConfigLogLevel(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "Debug";
        case LogLevel::Info: return "Info";
        case LogLevel::Warn: return "Warn";
        case LogLevel::Error: return "Error";
        default: return "Info";
    }
}

size_t ValueStart(const std::string& json, const std::string& key) {
    const size_t keyPos = json.find("\"" + key + "\"");
    if (keyPos == std::string::npos) return std::string::npos;
    const size_t colon = json.find(':', keyPos + key.size() + 2);
    if (colon == std::string::npos) return std::string::npos;
    return json.find_first_not_of(" \t\r\n", colon + 1);
}

bool ReadString(const std::string& json, const std::string& key, std::string* value) {
    size_t pos = ValueStart(json, key);
    if (pos == std::string::npos || json[pos] != '"') return false;
    ++pos;
    std::string output;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (escaped) {
            switch (c) {
                case 'n': output += '\n'; break;
                case 'r': output += '\r'; break;
                case 't': output += '\t'; break;
                case '\\': output += '\\'; break;
                case '"': output += '"'; break;
                default: return false;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            *value = std::move(output);
            return true;
        } else {
            output += c;
        }
    }
    return false;
}

bool ReadUInt16(const std::string& json, const std::string& key, uint16_t* value) {
    const size_t pos = ValueStart(json, key);
    if (pos == std::string::npos) return false;
    try {
        size_t consumed = 0;
        const unsigned long parsed = std::stoul(json.substr(pos), &consumed);
        if (consumed == 0 || parsed > 65535) return false;
        *value = static_cast<uint16_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool ReadBool(const std::string& json, const std::string& key, bool* value) {
    const size_t pos = ValueStart(json, key);
    if (pos == std::string::npos) return false;
    if (json.compare(pos, 4, "true") == 0) {
        *value = true;
        return true;
    }
    if (json.compare(pos, 5, "false") == 0) {
        *value = false;
        return true;
    }
    return false;
}

bool WriteConfig(const std::string& path, const RendezvousConfig& config,
                 std::string* error) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        *error = "Cannot create rendezvous config: " + path;
        return false;
    }
    output
        << "{\n"
        << "  \"bind_address\": \"" << EscapeJson(config.bindAddress) << "\",\n"
        << "  \"port\": " << config.port << ",\n"
        << "  \"auth_token\": \"" << EscapeJson(config.authToken) << "\",\n"
        << "  \"client_timeout_seconds\": " << config.clientTimeoutSeconds << ",\n"
        << "  \"max_clients_per_room\": " << config.maxClientsPerRoom << ",\n"
        << "  \"ipv4_relay_enabled\": "
        << (config.ipv4RelayEnabled ? "true" : "false") << ",\n"
        << "  \"ipv4_relay_port_start\": " << config.ipv4RelayPortStart << ",\n"
        << "  \"ipv4_relay_port_end\": " << config.ipv4RelayPortEnd << ",\n"
        << "  \"log_level\": \"" << ConfigLogLevel(config.logLevel) << "\",\n"
        << "  \"log_file\": \"" << EscapeJson(config.logFile) << "\"\n"
        << "}\n";
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
    std::ostringstream contents;
    contents << input.rdbuf();
    const std::string json = contents.str();
    if (json.find('{') == std::string::npos || json.find('}') == std::string::npos) {
        *error = "Invalid JSON object in " + path;
        return false;
    }

    std::string text;
    uint16_t number = 0;
    if (ReadString(json, "bind_address", &text)) config->bindAddress = text;
    if (ReadUInt16(json, "port", &number)) config->port = number;
    if (ReadString(json, "auth_token", &text)) config->authToken = text;
    if (ReadUInt16(json, "client_timeout_seconds", &number)) {
        config->clientTimeoutSeconds = number;
    }
    if (ReadUInt16(json, "max_clients_per_room", &number)) {
        config->maxClientsPerRoom = number;
    }
    ReadBool(json, "ipv4_relay_enabled", &config->ipv4RelayEnabled);
    if (ReadUInt16(json, "ipv4_relay_port_start", &number)) {
        config->ipv4RelayPortStart = number;
    }
    if (ReadUInt16(json, "ipv4_relay_port_end", &number)) {
        config->ipv4RelayPortEnd = number;
    }
    if (ReadString(json, "log_level", &text)
        && !TryParseLogLevel(text, &config->logLevel)) {
        *error = "Invalid log_level in " + path + ": " + text;
        return false;
    }
    if (ReadString(json, "log_file", &text)) config->logFile = text;

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
