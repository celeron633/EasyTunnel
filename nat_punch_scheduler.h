#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

struct NatPunchBatch {
    size_t offset = 0;
    size_t count = 0;
};

// Schedules a flattened socket x target wave. Time is supplied by the caller,
// allowing receive, cancellation and deadline checks between bounded batches.
class NatPunchScheduler {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    NatPunchScheduler(size_t waveSize, size_t batchSize,
                      uint32_t batchIntervalMs, uint32_t waveIntervalMs,
                      bool repeat);
    NatPunchBatch TakeBatch(TimePoint now, size_t remainingBudget);
    TimePoint nextSend() const { return nextSend_; }
    bool finished() const { return finished_; }

private:
    size_t waveSize_;
    size_t batchSize_;
    size_t cursor_ = 0;
    std::chrono::milliseconds batchInterval_;
    std::chrono::milliseconds waveInterval_;
    bool repeat_;
    bool finished_ = false;
    TimePoint waveStarted_{};
    TimePoint nextSend_{};
};

int NatPunchPollTimeoutMs(NatPunchScheduler::TimePoint now,
                          NatPunchScheduler::TimePoint nextEvent,
                          NatPunchScheduler::TimePoint deadline,
                          int maximumMs = 100);
