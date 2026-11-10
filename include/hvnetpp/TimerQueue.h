#pragma once

#include <memory>
#include <vector>
#include "hvnetpp/Timer.h"
#include "hvnetpp/TimerId.h"

namespace hvnetpp {

class EventLoop;

// 定时器队列：带索引的二叉最小堆 + 对象池，不使用 timerfd。
// EventLoop 用堆顶到期时间计算 epoll 超时，每轮 IO 之后调用 processExpired()。
// 稳态下增删定时器不分配内存（回调本身的 std::function 捕获超过 16 字节时除外），也没有系统调用。
class TimerQueue {
public:
    explicit TimerQueue(EventLoop* loop);
    ~TimerQueue();

    TimerQueue(const TimerQueue&) = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;

    // 线程安全。interval > 0 为周期定时器
    TimerId addTimer(TimerCallback cb, Timestamp when, std::chrono::nanoseconds interval);
    // 线程安全。取消尚未执行的定时器，包括本轮已到期但还没轮到执行的
    void cancel(TimerId timerId);

    // 以下仅限 loop 线程
    // 距最早定时器到期还有多少纳秒：已到期返回 0，无定时器或超过 maxNs 时返回 maxNs
    int64_t nextTimeoutNs(Timestamp now, int64_t maxNs) const;
    // 执行所有 expiration <= now 的定时器
    void processExpired(Timestamp now);
    size_t size() const { return heap_.size(); }

private:
    Timer* allocTimer();
    void releaseTimer(Timer* timer);
    void adoptTimer(std::unique_ptr<Timer> timer);
    void cancelInLoop(TimerId timerId);
    static void restart(Timer* timer, Timestamp now);

    static bool earlier(const Timer* a, const Timer* b);
    void heapPush(Timer* timer);
    void heapRemove(Timer* timer);
    void siftUp(size_t idx);
    void siftDown(size_t idx);
    void heapSet(size_t idx, Timer* timer);

    EventLoop* loop_;
    std::vector<Timer*> heap_;
    std::vector<std::unique_ptr<Timer>> owned_;   // 全部 Timer 对象（含空闲），析构时统一释放
    Timer* freeList_ = nullptr;
    std::vector<Timer*> expired_;                 // processExpired 复用的批次缓冲
};

} // namespace hvnetpp
