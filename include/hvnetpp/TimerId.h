#pragma once

#include "hvnetpp/Timer.h"

namespace hvnetpp {

class TimerId {
public:
    TimerId() : timer_(nullptr), sequence_(0) {}
    TimerId(Timer* timer, int64_t seq)
        : timer_(timer), sequence_(seq) {}

    friend class TimerQueue;

private:
    Timer* timer_;
    int64_t sequence_;
};

} // namespace hvnetpp
