#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

inline constexpr uint16_t kNatPunchProtocolVersionNumber = 3;
inline constexpr char kNatPunchProtocolVersion[] = "3";

std::string MakeControlMessage(const std::string& type,
                               const std::vector<std::string>& fields = {});
bool ParseControlMessage(const uint8_t* data, size_t len,
                         std::string* type, std::vector<std::string>* fields);
bool IsSafeControlField(const std::string& value);
