#pragma once

#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <sys/types.h>

#include "hvnetpp/TimerId.h"
#include "hvnetpp/TimerQueue.h"
#include "hvnetpp/MpscQueue.h"

namespace hvnetpp {

class Channel;
class Poller;
// class TimerQueue; // Moved to include header

class EventLoop {
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    void loop();
    void quit();

    void runInLoop(Functor cb);
    void queueInLoop(Functor cb);

    // Internal usage
    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);
    bool hasChannel(Channel* channel);

    bool isInLoopThread() const { return threadId_ == std::this_thread::get_id(); }
    void assertInLoopThread();

    // Timers (simplified interface)
    TimerId runAt(Timestamp time, TimerCallback cb);
    TimerId runAfter(double delay, TimerCallback cb);
    TimerId runEvery(double interval, TimerCallback cb);
    void cancel(TimerId timerId);

private:
    void wakeup();
    void handleRead(); // for wake up
    void doPendingFunctors();

    using ChannelList = std::vector<Channel*>;

    bool looping_; /* atomic */
    std::atomic<bool> quit_;
    bool eventHandling_;
    bool callingPendingFunctors_;
    const std::thread::id threadId_;
    const pid_t tid_;
    
    std::unique_ptr<Poller> poller_;
    std::unique_ptr<TimerQueue> timerQueue_;
    int wakeupFd_;
    std::unique_ptr<Channel> wakeupChannel_;
    
    ChannelList activeChannels_;
    Channel* currentActiveChannel_;

    std::mutex mutex_;
    std::vector<Functor> pendingFunctors_;
    struct FunctorTask {
        Functor* functorPtr;
    };
    using PendingQueue = MpscQueue<FunctorTask>;
    std::unique_ptr<PendingQueue> pendingQueue_;
};

} // namespace hvnetpp
