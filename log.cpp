#include "log.h"

#include <cctype>
#include <memory>
#include <mutex>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace {

std::once_flag g_loggerInitOnce;
std::shared_ptr<spdlog::logger> g_logger;
LogLevel g_minLogLevel = LogLevel::Info;
std::string g_logFilePath;
bool g_consoleLoggingEnabled = true;
std::mutex g_callbackMutex;
LogCallback g_logCallback;

spdlog::level::level_enum ToSpdlogLevel(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return spdlog::level::debug;
        case LogLevel::Info:
            return spdlog::level::info;
        case LogLevel::Warn:
            return spdlog::level::warn;
        case LogLevel::Error:
            return spdlog::level::err;
        default:
            return spdlog::level::info;
    }
}

std::shared_ptr<spdlog::logger> GetLogger() {
    std::call_once(g_loggerInitOnce, []() {
        std::vector<spdlog::sink_ptr> sinks;
        if (g_consoleLoggingEnabled) {
            sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        }
        if (!g_logFilePath.empty()) {
            try {
                sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                    g_logFilePath, true));
            } catch (...) {
                g_logFilePath.clear();
            }
        }
        if (sinks.empty()) {
            sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        }
        g_logger = std::make_shared<spdlog::logger>(
            "EasyTunnel", sinks.begin(), sinks.end());
        g_logger->set_pattern("%Y-%m-%d %H:%M:%S.%e %v");
        g_logger->set_level(ToSpdlogLevel(g_minLogLevel));
        g_logger->flush_on(spdlog::level::debug);
    });
    return g_logger;
}

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
    g_minLogLevel = level;
    GetLogger()->set_level(ToSpdlogLevel(level));
}

LogLevel GetLogLevel() {
    return g_minLogLevel;
}

void Log(LogLevel level, const std::string& msg) {
    if (!ShouldLog(level)) {
        return;
    }

    auto logger = GetLogger();
    const std::string text = "[" + std::string(LevelToString(level)) + "] " + msg;
    logger->log(ToSpdlogLevel(level), text);
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    if (g_logCallback) g_logCallback(level, text);
}

void SetLogCallback(LogCallback callback) {
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    g_logCallback = std::move(callback);
}

void SetLogFilePath(const std::string& path) {
    g_logFilePath = path;
}

void SetConsoleLoggingEnabled(bool enabled) {
    g_consoleLoggingEnabled = enabled;
}

std::string GetLogFilePath() {
    return g_logFilePath;
}
