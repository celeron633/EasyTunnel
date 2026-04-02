#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

void LogHexdumpDebug(const std::string& title, const uint8_t* data, size_t len, size_t maxBytes = 128);
