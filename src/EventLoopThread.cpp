#include "hvnetpp/EventLoopThread.h"
#include "hvnetpp/EventLoop.h"
#include "rtclog.h"

#include <cstring>
#include <pthread.h>
#include <sched.h>

namespace hvnetpp {

EventLoopThread::EventLoopThread(ThreadInitCallback cb, std::string name)
    : initCallback_(std::move(cb)),
      name_(std::move(name)) {
}

EventLoopThread::~EventLoopThread() {
    stop();
}

EventLoop* EventLoopThread::startLoop() {
    thread_ = std::thread(&EventLoopThread::threadFunc, this);

    EventLoop* loop = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this] { return loop_ != nullptr; });
        loop = loop_;
    }
    return loop;
}

void EventLoopThread::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        exiting_ = true;
        if (loop_) {
            // 必须持锁调用：线程函数在 loop() 返回后要先拿到这把锁把 loop_ 置空，之后才析构 EventLoop。
            // 若出锁后再 quit()，loop 线程可能恰好已退出并关闭 eventfd，wakeup() 会写到已关闭（甚至被复用）的 fd 上
            loop_->quit();
        }
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

EventLoop* EventLoopThread::loop() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loop_;
}

void EventLoopThread::threadFunc() {
    if (!name_.empty()) {
        // 便于 top -H / perf / gdb 区分线程；Linux 限制 15 字节
        pthread_setname_np(pthread_self(), name_.substr(0, 15).c_str());
    }
    if (cpu_ >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu_, &set);
        int err = pthread_setaffinity_np(pthread_self(), sizeof set, &set);
        if (err != 0) {
            RTCLOG(RTC_WARN, "EventLoopThread[%s] 绑定 CPU %d 失败: %s", name_.c_str(), cpu_, strerror(err));
        }
    }

    EventLoop loop;

    if (initCallback_) {
        initCallback_(&loop);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop_ = &loop;
    }
    cond_.notify_one();

    loop.loop();

    std::lock_guard<std::mutex> lock(mutex_);
    loop_ = nullptr;
}

} // namespace hvnetpp
