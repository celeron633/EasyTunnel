#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

std::string MakeControlMessage(const std::string& type,
                               const std::vector<std::string>& fields = {});
bool ParseControlMessage(const uint8_t* data, size_t len,
                         std::string* type, std::vector<std::string>* fields);
bool IsSafeControlField(const std::string& value);
