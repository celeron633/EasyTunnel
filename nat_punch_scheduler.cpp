#include "nat_punch_scheduler.h"

#include <algorithm>

NatPunchScheduler::NatPunchScheduler(
        size_t waveSize, size_t batchSize, uint32_t batchIntervalMs,
        uint32_t waveIntervalMs, bool repeat)
    : waveSize_(waveSize), batchSize_((std::max)(size_t{1}, batchSize)),
      batchInterval_((std::max)(uint32_t{1}, batchIntervalMs)),
      waveInterval_(waveIntervalMs), repeat_(repeat), finished_(waveSize == 0) {
    if (finished_) nextSend_ = TimePoint::max();
}

NatPunchBatch NatPunchScheduler::TakeBatch(TimePoint now, size_t remainingBudget) {
    if (finished_ || remainingBudget == 0 || now < nextSend_) return {};
    if (cursor_ == 0) waveStarted_ = now;
    const NatPunchBatch batch{
        cursor_, (std::min)({batchSize_, waveSize_ - cursor_, remainingBudget})};
    cursor_ += batch.count;
    // Schedule from the actual send time; never catch up in a burst after a stall.
    nextSend_ = now + batchInterval_;
    if (cursor_ == waveSize_) {
        if (repeat_) {
            cursor_ = 0;
            nextSend_ = (std::max)(nextSend_, waveStarted_ + waveInterval_);
        } else {
            finished_ = true;
            nextSend_ = TimePoint::max();
        }
    }
    return batch;
}

int NatPunchPollTimeoutMs(NatPunchScheduler::TimePoint now,
                          NatPunchScheduler::TimePoint nextEvent,
                          NatPunchScheduler::TimePoint deadline, int maximumMs) {
    const auto wake = (std::min)(nextEvent, deadline);
    if (wake <= now || maximumMs <= 0) return 0;
    const auto wait = std::chrono::ceil<std::chrono::milliseconds>(wake - now).count();
    return static_cast<int>((std::min)(wait, static_cast<int64_t>(maximumMs)));
}
