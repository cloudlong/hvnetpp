#pragma once

#include <atomic>
#include <functional>
#include <chrono>

namespace hvnetpp {

using Timestamp = std::chrono::steady_clock::time_point;
using TimeDelta = std::chrono::microseconds;
using TimerCallback = std::function<void()>;

class Timer {
public:
    Timer(TimerCallback cb, Timestamp when, TimeDelta interval)
        : callback_(std::move(cb)),
          expiration_(when),
          interval_(interval),
          repeat_(interval.count() > 0) {
    }

    void run() const {
        callback_();
    }

    Timestamp expiration() const { return expiration_; }
    TimeDelta interval() const { return interval_; }
    bool repeat() const { return repeat_; }
    int64_t sequence() const { return sequence_; }

    void restart(Timestamp now);

    static int64_t numCreated() { return s_numCreated_.load(std::memory_order_relaxed); }

private:
    const TimerCallback callback_;
    Timestamp expiration_;
    const TimeDelta interval_;
    const bool repeat_;
    const int64_t sequence_ = s_numCreated_.fetch_add(1, std::memory_order_relaxed) + 1;

    static std::atomic<int64_t> s_numCreated_;
};

} // namespace hvnetpp
