#pragma once

#include <functional>
#include <string>

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error
};

const char* LevelToString(LogLevel level);
bool TryParseLogLevel(const std::string& text, LogLevel* out);
void SetLogLevel(LogLevel level);
LogLevel GetLogLevel();
void Log(LogLevel level, const std::string& msg);

using LogCallback = std::function<void(LogLevel, const std::string&)>;
void SetLogCallback(LogCallback callback);
void SetLogFilePath(const std::string& path);
void SetConsoleLoggingEnabled(bool enabled);
std::string GetLogFilePath();
