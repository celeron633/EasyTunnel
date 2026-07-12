#include "nat_protocol.h"

#include <cstring>

std::string MakeControlMessage(const std::string& type,
                               const std::vector<std::string>& fields) {
    std::string out = "ETN1\t" + type;
    for (const auto& field : fields) out += "\t" + field;
    return out;
}

bool ParseControlMessage(const uint8_t* data, size_t len,
                         std::string* type, std::vector<std::string>* fields) {
    if (len < 6 || len > 8192 || std::memcmp(data, "ETN1\t", 5) != 0) return false;
    std::string input(reinterpret_cast<const char*>(data + 5), len - 5);
    fields->clear();
    size_t start = 0;
    size_t end = input.find('\t');
    *type = input.substr(0, end);
    while (end != std::string::npos) {
        start = end + 1;
        end = input.find('\t', start);
        fields->push_back(input.substr(start, end - start));
    }
    return !type->empty();
}

bool IsSafeControlField(const std::string& value) {
    return !value.empty() && value.size() <= 128
        && value.find_first_of("\t\r\n") == std::string::npos;
}
