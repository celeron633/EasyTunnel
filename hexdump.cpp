#include "hexdump.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

#include "log.h"

void LogHexdumpDebug(const std::string& title, const uint8_t* data, size_t len, size_t maxBytes) {
    if (data == nullptr || len == 0) {
        Log(LogLevel::Debug, title + ", hexdump: <empty>");
        return;
    }

    const size_t dumpLen = (std::min)(len, maxBytes);
    Log(LogLevel::Debug, title + ", hexdump bytes=" + std::to_string(dumpLen) + "/" + std::to_string(len));

    for (size_t i = 0; i < dumpLen; i += 16) {
        std::ostringstream line;
        line << std::setfill('0') << std::setw(8) << std::hex << std::nouppercase << i << "  ";

        for (size_t j = 0; j < 16; ++j) {
            if (i + j < dumpLen) {
                line << std::setfill('0') << std::setw(2) << std::hex << static_cast<unsigned>(data[i + j]) << ' ';
            } else {
                line << "   ";
            }
            if (j == 7) {
                line << ' ';
            }
        }

        line << " |";
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < dumpLen) {
                const unsigned char c = data[i + j];
                line << (std::isprint(c) ? static_cast<char>(c) : '.');
            } else {
                line << ' ';
            }
        }
        line << '|';

        Log(LogLevel::Debug, line.str());
    }

    if (dumpLen < len) {
        Log(LogLevel::Debug, "hexdump truncated (max " + std::to_string(maxBytes) + " bytes)");
    }
}
