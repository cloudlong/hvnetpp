#include "hvnetpp/TimerQueue.h"
#include "hvnetpp/EventLoop.h"
#include "rtclog.h"
#include <cstdlib>
#include <sys/timerfd.h>
#include <unistd.h>
#include <cstring>
#include <assert.h>

namespace hvnetpp {

int createTimerfd() {
    int timerfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd < 0) {
        RTCLOG(RTC_FATAL, "Failed in createTimerfd error: %s", strerror(errno));
        std::abort();
    }
    return timerfd;
}

struct timespec howMuchTimeFromNow(Timestamp when) {
    auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(when - std::chrono::steady_clock::now()).count();
    if (microseconds < 100) microseconds = 100;
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(microseconds / 1000000);
    ts.tv_nsec = static_cast<long>((microseconds % 1000000) * 1000);
    return ts;
}

void resetTimerfd(int timerfd, Timestamp expiration) {
    struct itimerspec newValue;
    struct itimerspec oldValue;
    memset(&newValue, 0, sizeof newValue);
    memset(&oldValue, 0, sizeof oldValue);
    newValue.it_value = howMuchTimeFromNow(expiration);
    int ret = ::timerfd_settime(timerfd, 0, &newValue, &oldValue);
    if (ret) {
        RTCLOG(RTC_ERROR, "timerfd_settime() error: %s", strerror(errno));
    }
}

void readTimerfd(int timerfd, Timestamp now) {
    uint64_t howmany;
    ssize_t n = ::read(timerfd, &howmany, sizeof howmany);
    if (n != sizeof howmany) {
        RTCLOG(RTC_ERROR, "TimerQueue::handleRead() reads %zd bytes instead of 8", n);
    }
}

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop),
      timerfd_(createTimerfd()),
      timerfdChannel_(loop, timerfd_),
      timers_(),
      callingExpiredTimers_(false) {
    timerfdChannel_.setReadCallback(std::bind(&TimerQueue::handleRead, this));
    timerfdChannel_.enableReading();
}

TimerQueue::~TimerQueue() {
    timerfdChannel_.disableAll();
    timerfdChannel_.remove();
    ::close(timerfd_);
    for (const auto& timer : timers_) {
        delete timer.second;
    }
}

TimerId TimerQueue::addTimer(TimerCallback cb, Timestamp when, double interval) {
    Timer* timer = new Timer(std::move(cb), when, interval);
    loop_->runInLoop(std::bind(&TimerQueue::addTimerInLoop, this, timer));
    return TimerId(timer, timer->sequence());
}

void TimerQueue::cancel(TimerId timerId) {
    loop_->runInLoop(std::bind(&TimerQueue::cancelInLoop, this, timerId));
}

void TimerQueue::addTimerInLoop(Timer* timer) {
    loop_->assertInLoopThread();
    bool earliestChanged = insert(timer);
    if (earliestChanged) {
        resetTimerfd(timerfd_, timer->expiration());
    }
}

void TimerQueue::cancelInLoop(TimerId timerId) {
    loop_->assertInLoopThread();
    assert(timers_.size() == activeTimers_.size());
    ActiveTimer timer(timerId.timer_, timerId.sequence_);
    auto it = activeTimers_.find(timer);
    if (it != activeTimers_.end()) {
        size_t n = timers_.erase(Entry(it->first->expiration(), it->first));
        assert(n == 1);
        delete it->first;
        activeTimers_.erase(it);
    } else if (callingExpiredTimers_) {
        cancelingTimers_.insert(timer);
    }
}

void TimerQueue::handleRead() {
    loop_->assertInLoopThread();
    Timestamp now = std::chrono::steady_clock::now();
    readTimerfd(timerfd_, now);

    std::vector<Entry> expired = getExpired(now);

    callingExpiredTimers_ = true;
    cancelingTimers_.clear();
    for (const auto& entry : expired) {
        entry.second->run();
    }
    callingExpiredTimers_ = false;

    reset(expired, now);
}

std::vector<TimerQueue::Entry> TimerQueue::getExpired(Timestamp now) {
    std::vector<Entry> expired;
    Entry sentry(now, reinterpret_cast<Timer*>(UINTPTR_MAX));
    auto end = timers_.lower_bound(sentry);
    std::copy(timers_.begin(), end, std::back_inserter(expired));
    timers_.erase(timers_.begin(), end);

    for (const auto& entry : expired) {
        ActiveTimer timer(entry.second, entry.second->sequence());
        size_t n = activeTimers_.erase(timer);
        assert(n == 1);
    }
    return expired;
}

void TimerQueue::reset(const std::vector<Entry>& expired, Timestamp now) {
    Timestamp nextExpire;
    for (const auto& entry : expired) {
        ActiveTimer timer(entry.second, entry.second->sequence());
        if (entry.second->repeat() && cancelingTimers_.find(timer) == cancelingTimers_.end()) {
            entry.second->restart(now);
            insert(entry.second);
        } else {
            delete entry.second;
        }
    }

    if (!timers_.empty()) {
        nextExpire = timers_.begin()->second->expiration();
    }

    if (nextExpire != Timestamp()) {
        resetTimerfd(timerfd_, nextExpire);
    }
}

bool TimerQueue::insert(Timer* timer) {
    loop_->assertInLoopThread();
    assert(timers_.size() == activeTimers_.size());
    bool earliestChanged = false;
    Timestamp when = timer->expiration();
    auto it = timers_.begin();
    if (it == timers_.end() || when < it->first) {
        earliestChanged = true;
    }
    {
        std::pair<TimerList::iterator, bool> result = timers_.insert(Entry(when, timer));
        assert(result.second);
    }
    {
        std::pair<ActiveTimerSet::iterator, bool> result = activeTimers_.insert(ActiveTimer(timer, timer->sequence()));
        assert(result.second);
    }
    assert(timers_.size() == activeTimers_.size());
    return earliestChanged;
}

} // namespace hvnetpp
