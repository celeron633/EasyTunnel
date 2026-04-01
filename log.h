#pragma once

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
