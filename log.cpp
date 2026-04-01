#include "log.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>

namespace {

std::mutex g_logMutex;

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

void Log(LogLevel level, const std::string& msg) {
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
