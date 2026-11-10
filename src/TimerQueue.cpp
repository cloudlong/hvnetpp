#include "hvnetpp/TimerQueue.h"
#include "hvnetpp/EventLoop.h"

#include <assert.h>
#include <atomic>

namespace hvnetpp {

namespace {
std::atomic<int64_t> s_timerSeq{0};
}

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop) {
}

TimerQueue::~TimerQueue() {
    // 先把回调移出来再统一析构：回调捕获的对象析构时可能回调到本队列（此时只会走 cancel 的空操作分支）
    std::vector<TimerCallback> callbacks;
    callbacks.reserve(owned_.size());
    for (auto& t : owned_) {
        t->sequence = 0;
        t->state = Timer::kFree;
        callbacks.push_back(std::move(t->callback));
    }
    heap_.clear();
    callbacks.clear();
    owned_.clear();
}

TimerId TimerQueue::addTimer(TimerCallback cb, Timestamp when, std::chrono::nanoseconds interval) {
    const int64_t seq = s_timerSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    if (loop_->isInLoopThread()) {
        Timer* t = allocTimer();
        t->callback = std::move(cb);
        t->expiration = when;
        t->interval = interval;
        t->sequence = seq;
        t->cancelled = false;
        t->state = Timer::kActive;
        heapPush(t);
        return TimerId(t, seq);
    }
    // 跨线程：先在调用方线程 new 出来，交给 loop 线程收编进对象池
    std::unique_ptr<Timer> t(new Timer);
    t->callback = std::move(cb);
    t->expiration = when;
    t->interval = interval;
    t->sequence = seq;
    t->state = Timer::kPending;
    TimerId id(t.get(), seq);
    loop_->queueInLoop([this, t = std::move(t)]() mutable { adoptTimer(std::move(t)); });
    return id;
}

void TimerQueue::cancel(TimerId timerId) {
    if (!timerId.valid()) {
        return;
    }
    loop_->runInLoop([this, timerId] { cancelInLoop(timerId); });
}

void TimerQueue::adoptTimer(std::unique_ptr<Timer> timer) {
    Timer* t = timer.get();
    owned_.push_back(std::move(timer));
    if (t->cancelled) {
        releaseTimer(t);
        return;
    }
    t->state = Timer::kActive;
    heapPush(t);
}

void TimerQueue::cancelInLoop(TimerId timerId) {
    Timer* t = timerId.timer_;
    if (t == nullptr || t->sequence != timerId.sequence_) {
        return;   // 已执行完或已取消（对象可能已被复用给别的定时器）
    }
    switch (t->state) {
    case Timer::kActive:
        heapRemove(t);
        releaseTimer(t);
        break;
    case Timer::kRunning:   // 本轮待执行：标记后由 processExpired 跳过并回收
    case Timer::kPending:   // 尚未被 loop 收编：收编时回收
        t->cancelled = true;
        break;
    case Timer::kFree:
        break;
    }
}

int64_t TimerQueue::nextTimeoutNs(Timestamp now, int64_t maxNs) const {
    if (heap_.empty()) {
        return maxNs;
    }
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(heap_[0]->expiration - now).count();
    if (ns <= 0) {
        return 0;
    }
    return ns < maxNs ? ns : maxNs;
}

void TimerQueue::processExpired(Timestamp now) {
    if (heap_.empty() || heap_[0]->expiration > now) {
        return;
    }
    // 先摘出整批到期定时器再执行：回调里新加的定时器（即便 delay 为 0）留到下一轮，保证本轮有界
    expired_.clear();
    while (!heap_.empty() && heap_[0]->expiration <= now) {
        Timer* t = heap_[0];
        heapRemove(t);
        t->state = Timer::kRunning;
        expired_.push_back(t);
    }
    for (size_t i = 0; i < expired_.size(); ++i) {
        Timer* t = expired_[i];
        if (!t->cancelled) {
            t->callback();
        }
        if (t->interval.count() > 0 && !t->cancelled) {
            restart(t, now);
            t->state = Timer::kActive;
            heapPush(t);
        } else {
            releaseTimer(t);
        }
    }
    expired_.clear();
}

void TimerQueue::restart(Timer* t, Timestamp now) {
    t->expiration += t->interval;
    if (t->expiration <= now) {
        // 落后太多（loop 被阻塞过）：跳过错过的周期，不补发
        const auto skipped = (now - t->expiration) / t->interval + 1;
        t->expiration += t->interval * skipped;
    }
}

Timer* TimerQueue::allocTimer() {
    if (freeList_ != nullptr) {
        Timer* t = freeList_;
        freeList_ = t->nextFree;
        t->nextFree = nullptr;
        return t;
    }
    owned_.emplace_back(new Timer);
    return owned_.back().get();
}

void TimerQueue::releaseTimer(Timer* t) {
    // 先把回调移出并让 t 完整进入空闲链表，再析构回调：
    // 回调捕获对象的析构函数可能再次 addTimer，甚至复用到 t 本身
    TimerCallback dead(std::move(t->callback));
    t->callback = nullptr;
    t->sequence = 0;
    t->cancelled = false;
    t->state = Timer::kFree;
    t->nextFree = freeList_;
    freeList_ = t;
}

// ---------------- 最小堆 ----------------

bool TimerQueue::earlier(const Timer* a, const Timer* b) {
    if (a->expiration != b->expiration) {
        return a->expiration < b->expiration;
    }
    return a->sequence < b->sequence;   // 同一时刻按创建顺序执行
}

void TimerQueue::heapSet(size_t idx, Timer* t) {
    heap_[idx] = t;
    t->heapIndex = idx;
}

void TimerQueue::heapPush(Timer* t) {
    heap_.push_back(t);
    t->heapIndex = heap_.size() - 1;
    siftUp(t->heapIndex);
}

void TimerQueue::heapRemove(Timer* t) {
    const size_t idx = t->heapIndex;
    assert(idx < heap_.size() && heap_[idx] == t);
    Timer* last = heap_.back();
    heap_.pop_back();
    if (last != t) {
        heapSet(idx, last);
        siftDown(idx);
        siftUp(last->heapIndex);
    }
}

void TimerQueue::siftUp(size_t idx) {
    Timer* t = heap_[idx];
    while (idx > 0) {
        const size_t parent = (idx - 1) / 2;
        if (!earlier(t, heap_[parent])) {
            break;
        }
        heapSet(idx, heap_[parent]);
        idx = parent;
    }
    heapSet(idx, t);
}

void TimerQueue::siftDown(size_t idx) {
    const size_t n = heap_.size();
    Timer* t = heap_[idx];
    while (true) {
        size_t child = idx * 2 + 1;
        if (child >= n) {
            break;
        }
        if (child + 1 < n && earlier(heap_[child + 1], heap_[child])) {
            ++child;
        }
        if (!earlier(heap_[child], t)) {
            break;
        }
        heapSet(idx, heap_[child]);
        idx = child;
    }
    heapSet(idx, t);
}

} // namespace hvnetpp
