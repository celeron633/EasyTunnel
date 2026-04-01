#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

std::string HexdumpC(const uint8_t* data, size_t len, size_t baseOffset = 0);
