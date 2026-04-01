#include "hexdump.h"

#include <cctype>
#include <iomanip>
#include <sstream>

std::string HexdumpC(const uint8_t* data, size_t len, size_t baseOffset) {
    std::ostringstream out;

    for (size_t i = 0; i < len; i += 16) {
        out << std::setfill('0') << std::setw(8) << std::hex << std::nouppercase << (baseOffset + i) << "  ";

        for (size_t j = 0; j < 16; ++j) {
            if (i + j < len) {
                out << std::setfill('0') << std::setw(2) << std::hex << static_cast<unsigned>(data[i + j]) << ' ';
            } else {
                out << "   ";
            }
            if (j == 7) {
                out << ' ';
            }
        }

        out << " |";
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < len) {
                const unsigned char c = data[i + j];
                out << (std::isprint(c) ? static_cast<char>(c) : '.');
            } else {
                out << ' ';
            }
        }
        out << '|';

        if (i + 16 < len) {
            out << '\n';
        }
    }

    return out.str();
}
