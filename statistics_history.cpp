#include "statistics_history.h"

#include <algorithm>

void StatisticsHistory::Update(uint64_t txBytes, uint64_t rxBytes,
                               int64_t latencyMilliseconds) {
    const auto now = std::chrono::steady_clock::now();
    if (!initialized_) {
        initialized_ = true;
        previousTxBytes_ = txBytes;
        previousRxBytes_ = rxBytes;
        previousSampleTime_ = now;
        return;
    }

    const double elapsed = std::chrono::duration<double>(now - previousSampleTime_).count();
    if (elapsed < 5.0) return;

    const uint64_t txDelta = txBytes >= previousTxBytes_
        ? txBytes - previousTxBytes_ : txBytes;
    const uint64_t rxDelta = rxBytes >= previousRxBytes_
        ? rxBytes - previousRxBytes_ : rxBytes;
    samples_.push_back({
        static_cast<float>(static_cast<double>(txDelta) / elapsed / 1024.0),
        static_cast<float>(static_cast<double>(rxDelta) / elapsed / 1024.0),
        static_cast<float>((std::max)(int64_t{0}, latencyMilliseconds)),
        std::chrono::system_clock::now(),
    });
    if (samples_.size() > kMaxSamples) samples_.erase(samples_.begin());

    previousTxBytes_ = txBytes;
    previousRxBytes_ = rxBytes;
    previousSampleTime_ = now;
}
