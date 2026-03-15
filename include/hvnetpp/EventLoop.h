#pragma once

#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <sys/types.h>

#include "hvnetpp/TimerId.h"
#include "hvnetpp/TimerQueue.h"
#include "hvnetpp/MpscQueue.h"

namespace hvnetpp {

class Channel;
class Poller;
class TcpConnection;
class UdpSocket;
class Acceptor;

class EventLoop {
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    // Must be called on the owning thread and blocks until quit().
    void loop();
    // Safe to call from any thread.
    void quit();

    // Safe to call from any thread. Runs immediately on the loop thread, otherwise queues.
    void runInLoop(Functor cb);
    // Safe to call from any thread. Always queues for later execution on the loop thread.
    void queueInLoop(Functor cb);

    bool isInLoopThread() const { return threadId_ == std::this_thread::get_id(); }

    // Safe to call from any thread.
    TimerId runAt(Timestamp time, TimerCallback cb);
    // Safe to call from any thread. Compatibility overload using seconds.
    TimerId runAfter(double delay, TimerCallback cb);
    // Safe to call from any thread.
    TimerId runAfter(TimeDelta delay, TimerCallback cb);
    template <class Rep, class Period>
    TimerId runAfter(const std::chrono::duration<Rep, Period>& delay, TimerCallback cb) {
        return runAfter(std::chrono::duration_cast<TimeDelta>(delay), std::move(cb));
    }

    // Safe to call from any thread. Compatibility overload using seconds.
    TimerId runEvery(double interval, TimerCallback cb);
    // Safe to call from any thread.
    TimerId runEvery(TimeDelta interval, TimerCallback cb);
    template <class Rep, class Period>
    TimerId runEvery(const std::chrono::duration<Rep, Period>& interval, TimerCallback cb) {
        return runEvery(std::chrono::duration_cast<TimeDelta>(interval), std::move(cb));
    }
    // Safe to call from any thread.
    void cancel(TimerId timerId);

private:
    friend class Channel;
    friend class TimerQueue;
    friend class TcpConnection;
    friend class UdpSocket;
    friend class Acceptor;

    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);
    bool hasChannel(Channel* channel);
    void assertInLoopThread();
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
