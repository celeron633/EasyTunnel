#pragma once

#include <string>

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error
};

const char* LevelToString(LogLevel level);
void Log(LogLevel level, const std::string& msg);
