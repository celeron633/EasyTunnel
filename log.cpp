#include "log.h"

#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>

namespace {

std::mutex g_logMutex;
LogLevel g_minLogLevel = LogLevel::Info;

std::string ToLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool ShouldLog(LogLevel level) {
    return static_cast<int>(level) >= static_cast<int>(g_minLogLevel);
}

}  // namespace

const char* LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO ";
        case LogLevel::Warn:
            return "WARN ";
        case LogLevel::Error:
            return "ERROR";
        default:
            return "UNKWN";
    }
}

bool TryParseLogLevel(const std::string& text, LogLevel* out) {
    if (out == nullptr) {
        return false;
    }

    const std::string t = ToLowerAscii(text);
    if (t == "debug") {
        *out = LogLevel::Debug;
        return true;
    }
    if (t == "info") {
        *out = LogLevel::Info;
        return true;
    }
    if (t == "warn") {
        *out = LogLevel::Warn;
        return true;
    }
    if (t == "error") {
        *out = LogLevel::Error;
        return true;
    }
    return false;
}

void SetLogLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_minLogLevel = level;
}

LogLevel GetLogLevel() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_minLogLevel;
}

void Log(LogLevel level, const std::string& msg) {
    if (!ShouldLog(level)) {
        return;
    }

    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm localTm{};
    localtime_s(&localTm, &t);

    std::lock_guard<std::mutex> lock(g_logMutex);
    std::cout << std::put_time(&localTm, "%Y-%m-%d %H:%M:%S")
              << '.' << std::setfill('0') << std::setw(3) << ms.count()
              << " [" << LevelToString(level) << "] " << msg << std::endl;
}
