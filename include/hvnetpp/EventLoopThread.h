#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace hvnetpp {

class EventLoop;

// 拥有一个线程，线程内跑一个 EventLoop（one loop per thread）
class EventLoopThread {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    explicit EventLoopThread(ThreadInitCallback cb = ThreadInitCallback(),
                             std::string name = std::string());
    ~EventLoopThread();

    EventLoopThread(const EventLoopThread&) = delete;
    EventLoopThread& operator=(const EventLoopThread&) = delete;

    // 启动线程并阻塞等待 loop 就绪，返回 loop 指针
    EventLoop* startLoop();
    // quit + join；可重复调用
    void stop();

    // 未启动或已停止时返回 nullptr
    EventLoop* loop() const;
    const std::string& name() const { return name_; }

private:
    void threadFunc();

    EventLoop* loop_ = nullptr;
    bool exiting_ = false;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    ThreadInitCallback initCallback_;
    std::string name_;
};

} // namespace hvnetpp
