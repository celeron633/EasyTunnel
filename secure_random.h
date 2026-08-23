#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

bool FillSecureRandom(uint8_t* data, size_t len, std::string* error);
std::string SecureRandomHex(size_t byteCount, std::string* error);

