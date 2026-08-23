#include "secure_random.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#elif defined(__linux__)
#include <cerrno>
#include <cstring>
#include <sys/random.h>
#else
#include <random>
#endif

namespace {
void SetError(std::string* error, const std::string& value) {
    if (error != nullptr) *error = value;
}
}  // namespace

bool FillSecureRandom(uint8_t* data, size_t len, std::string* error) {
    if (data == nullptr && len != 0) {
        SetError(error, "Invalid secure-random output buffer");
        return false;
    }
#ifdef _WIN32
    size_t offset = 0;
    while (offset < len) {
        const size_t remaining = len - offset;
        const ULONG chunk = static_cast<ULONG>((std::min)(
            remaining, static_cast<size_t>((std::numeric_limits<ULONG>::max)())));
        const NTSTATUS status = BCryptGenRandom(
            nullptr, data + offset, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) {
            SetError(error, "BCryptGenRandom failed. status="
                + std::to_string(static_cast<long>(status)));
            return false;
        }
        offset += chunk;
    }
#elif defined(__linux__)
    size_t offset = 0;
    while (offset < len) {
        const ssize_t received = getrandom(data + offset, len - offset, 0);
        if (received > 0) {
            offset += static_cast<size_t>(received);
            continue;
        }
        if (received < 0 && errno == EINTR) continue;
        SetError(error, "getrandom failed: " + std::string(std::strerror(errno)));
        return false;
    }
#else
    std::random_device random;
    for (size_t i = 0; i < len; ++i) {
        data[i] = static_cast<uint8_t>(random());
    }
#endif
    return true;
}

std::string SecureRandomHex(size_t byteCount, std::string* error) {
    std::vector<uint8_t> bytes(byteCount);
    if (!FillSecureRandom(bytes.data(), bytes.size(), error)) return {};
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const uint8_t byte : bytes) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

