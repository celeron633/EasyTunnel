#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

struct StatisticsSample {
    float txKibPerSecond = 0.0f;
    float rxKibPerSecond = 0.0f;
    float latencyMilliseconds = 0.0f;
    std::chrono::system_clock::time_point timestamp{};
};

class StatisticsHistory {
public:
    static constexpr std::size_t kMaxSamples = 60;

    void Update(uint64_t txBytes, uint64_t rxBytes, int64_t latencyMilliseconds);
    const std::vector<StatisticsSample>& Samples() const { return samples_; }

private:
    bool initialized_ = false;
    uint64_t previousTxBytes_ = 0;
    uint64_t previousRxBytes_ = 0;
    std::chrono::steady_clock::time_point previousSampleTime_{};
    std::vector<StatisticsSample> samples_;
};
