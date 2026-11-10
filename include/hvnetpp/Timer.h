#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace hvnetpp {

using Timestamp = std::chrono::steady_clock::time_point;
using TimerCallback = std::function<void()>;

// 定时器条目（TimerQueue 内部使用）。
// Timer 对象在所属 TimerQueue 的生命周期内只回收复用、从不释放，
// 因此过期的 TimerId 解引用仍是安全的，再用 sequence 判断是否还是同一个定时器。
struct Timer {
    enum State : uint8_t {
        kFree,      // 在空闲链表中
        kPending,   // 其他线程创建，尚未进入 loop 线程的堆
        kActive,    // 在堆中等待到期
        kRunning,   // 已到期，处于本轮待执行批次中
    };

    TimerCallback callback;
    Timestamp expiration;
    std::chrono::nanoseconds interval{0};   // > 0 为周期定时器
    int64_t sequence = 0;                   // 全局唯一，0 表示空闲
    size_t heapIndex = 0;
    State state = kFree;
    bool cancelled = false;
    Timer* nextFree = nullptr;
};

} // namespace hvnetpp
