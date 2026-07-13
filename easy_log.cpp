#include "log.h"

#include <chrono>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <utility>

namespace {

LogLevel g_minLogLevel = LogLevel::Info;
std::string g_logFilePath;
bool g_consoleLoggingEnabled = true;
std::mutex g_logMutex;
LogCallback g_logCallback;

std::string ToLowerAscii(const std::string& value) {
    std::string result = value;
    for (char& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

std::string Timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << '.'
           << std::setfill('0') << std::setw(3) << milliseconds.count();
    return output.str();
}

}  // namespace

const char* LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO ";
        case LogLevel::Warn: return "WARN ";
        case LogLevel::Error: return "ERROR";
        default: return "UNKWN";
    }
}

bool TryParseLogLevel(const std::string& text, LogLevel* out) {
    if (out == nullptr) return false;
    const std::string value = ToLowerAscii(text);
    if (value == "debug") *out = LogLevel::Debug;
    else if (value == "info") *out = LogLevel::Info;
    else if (value == "warn") *out = LogLevel::Warn;
    else if (value == "error") *out = LogLevel::Error;
    else return false;
    return true;
}

void SetLogLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_minLogLevel = level;
}

LogLevel GetLogLevel() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_minLogLevel;
}

void Log(LogLevel level, const std::string& message) {
    LogCallback callback;
    std::string text;
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (static_cast<int>(level) < static_cast<int>(g_minLogLevel)) return;

        text = "[" + std::string(LevelToString(level)) + "] " + message;
        const std::string line = Timestamp() + " " + text;
        if (g_consoleLoggingEnabled) std::cout << line << std::endl;
        if (!g_logFilePath.empty()) {
            std::ofstream file(g_logFilePath, std::ios::app);
            if (file.is_open()) file << line << '\n';
        }
        callback = g_logCallback;
    }
    if (callback) callback(level, text);
}

void SetLogCallback(LogCallback callback) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logCallback = std::move(callback);
}

void SetLogFilePath(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logFilePath = path;
}

void SetConsoleLoggingEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_consoleLoggingEnabled = enabled;
}

std::string GetLogFilePath() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_logFilePath;
}
