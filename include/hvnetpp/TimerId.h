#pragma once

#include "hvnetpp/Timer.h"

namespace hvnetpp {

class TimerId {
public:
    TimerId() : timer_(nullptr), sequence_(0) {}
    TimerId(Timer* timer, int64_t seq)
        : timer_(timer), sequence_(seq) {}

    bool valid() const { return timer_ != nullptr; }
    explicit operator bool() const { return valid(); }

    friend class TimerQueue;

private:
    Timer* timer_;
    int64_t sequence_;
};

} // namespace hvnetpp
