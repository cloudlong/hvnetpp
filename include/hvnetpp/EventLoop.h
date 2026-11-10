#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <sys/types.h>

#include "hvnetpp/MpscQueue.h"
#include "hvnetpp/TimerId.h"

namespace hvnetpp {

class Channel;
class Poller;
class TimerQueue;

// one loop per thread：epoll（LT）+ 定时器堆 + eventfd 唤醒 + 无锁任务队列
class EventLoop {
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void loop();
    void quit();   // 线程安全

    // ---- 任务投递（线程安全）----
    // 同一线程投递的任务严格按投递顺序执行。
    // 任务在 loop 线程执行；执行任务时新投递的任务留到下一轮，不会饿死 IO。
    // 稳态开销：一次内存分配 + 一次原子 exchange；跨线程投递只有队列由空变非空时才写 eventfd。
    template <typename F> void runInLoop(F&& f);     // 已在 loop 线程则立即执行
    template <typename F> void queueInLoop(F&& f);   // 总是入队
    // 在 loop 线程同步执行 f 并等待完成（已在 loop 线程则直接执行），返回 f 是否被执行。
    // 若 loop 恰好已退出、任务被丢弃，返回 false 而不是永久阻塞（loop 对象必须仍然存活）。
    // 用于启动/停止等低频场景；两个 loop 线程互相同步等待会死锁。
    template <typename F> bool runInLoopAndWait(F&& f);

    // ---- 定时器（线程安全）----
    // double 版本单位为秒；也可直接传 std::chrono::duration，如 runEvery(std::chrono::milliseconds(5), cb)
    TimerId runAt(Timestamp time, TimerCallback cb);
    TimerId runAfter(double delaySeconds, TimerCallback cb);
    TimerId runEvery(double intervalSeconds, TimerCallback cb);
    template <typename Rep, typename Period>
    TimerId runAfter(std::chrono::duration<Rep, Period> delay, TimerCallback cb) {
        return runAt(std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::nanoseconds>(delay),
                     std::move(cb));
    }
    template <typename Rep, typename Period>
    TimerId runEvery(std::chrono::duration<Rep, Period> interval, TimerCallback cb) {
        return runEveryNs(std::chrono::duration_cast<std::chrono::nanoseconds>(interval), std::move(cb));
    }
    void cancel(TimerId timerId);

    // ---- 内部使用 ----
    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);
    bool hasChannel(Channel* channel);

    bool isInLoopThread() const { return threadId_ == std::this_thread::get_id(); }
    void assertInLoopThread();

    // 当前线程的 EventLoop（不在 loop 线程返回 nullptr）
    static EventLoop* getEventLoopOfCurrentThread();

private:
    struct Task : MpscNode {
        void (*invoke)(Task* self, bool run);   // 执行（run=true）或仅销毁，之后释放自身
    };
    template <typename F>
    struct TaskImpl : Task {
        F fn;
        template <typename G>
        explicit TaskImpl(G&& g) : fn(std::forward<G>(g)) { invoke = &TaskImpl::call; }
        static void call(Task* t, bool run) {
            std::unique_ptr<TaskImpl> self(static_cast<TaskImpl*>(t));
            if (run) {
                self->fn();
            }
        }
    };

    void enqueue(Task* task);
    void wakeup();
    void handleRead();   // eventfd 可读
    void doPendingFunctors();
    TimerId runEveryNs(std::chrono::nanoseconds interval, TimerCallback cb);

    using ChannelList = std::vector<Channel*>;

    bool looping_;
    std::atomic<bool> quit_;
    bool eventHandling_;      // 处于 IO/定时器回调阶段（之后必然执行 doPendingFunctors）
    bool rerunPending_;       // loop 线程内投递了本轮来不及执行的任务：下次 poll 不阻塞
    const std::thread::id threadId_;
    const pid_t tid_;

    std::unique_ptr<Poller> poller_;
    std::unique_ptr<TimerQueue> timerQueue_;
    int wakeupFd_;
    std::unique_ptr<Channel> wakeupChannel_;

    ChannelList activeChannels_;
    Channel* currentActiveChannel_;

    MpscQueue pendingQueue_;
    std::atomic<bool> wakeupPending_;   // 已写过 eventfd、消费者尚未开始处理
};

template <typename F>
void EventLoop::queueInLoop(F&& f) {
    using Fn = typename std::decay<F>::type;
    enqueue(new TaskImpl<Fn>(std::forward<F>(f)));
}

template <typename F>
void EventLoop::runInLoop(F&& f) {
    if (isInLoopThread()) {
        f();
    } else {
        queueInLoop(std::forward<F>(f));
    }
}

template <typename F>
bool EventLoop::runInLoopAndWait(F&& f) {
    if (isInLoopThread()) {
        f();
        return true;
    }
    struct State {
        std::mutex mutex;
        std::condition_variable cond;
        bool done = false;
        bool ran = false;
    };
    // 随任务一起销毁时通知等待方：任务执行完、或因 loop 退出被 ~EventLoop 直接丢弃，都会走到这里
    struct Notifier {
        State* st;
        explicit Notifier(State* s) : st(s) {}
        Notifier(Notifier&& other) : st(other.st) { other.st = nullptr; }
        Notifier(const Notifier&) = delete;
        Notifier& operator=(const Notifier&) = delete;
        ~Notifier() {
            if (st) {
                std::lock_guard<std::mutex> lock(st->mutex);
                st->done = true;
                st->cond.notify_one();
            }
        }
    };
    State st;
    queueInLoop([&f, &st, notifier = Notifier(&st)]() mutable {
        f();
        st.ran = true;   // 随后 Notifier 析构时加锁发布
    });
    std::unique_lock<std::mutex> lock(st.mutex);
    st.cond.wait(lock, [&st] { return st.done; });
    return st.ran;
}

} // namespace hvnetpp
