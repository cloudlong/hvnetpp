#pragma once

#include "hvnetpp/Timer.h"

namespace hvnetpp {

// 定时器句柄：可拷贝，取消已触发/已取消的定时器是安全的空操作
class TimerId {
public:
    TimerId() : timer_(nullptr), sequence_(0) {}
    TimerId(Timer* timer, int64_t seq)
        : timer_(timer), sequence_(seq) {}

    bool valid() const { return timer_ != nullptr; }

    friend class TimerQueue;

private:
    Timer* timer_;
    int64_t sequence_;
};

} // namespace hvnetpp
