#pragma once

#include "hvnetpp/EventLoopThread.h"

#include <memory>
#include <string>
#include <vector>

namespace hvnetpp {

class EventLoop;

// EventLoop 线程池；numThreads == 0 时 getNextLoop() 返回 baseLoop
class EventLoopThreadPool {
public:
    EventLoopThreadPool(EventLoop* baseLoop, std::string name);
    ~EventLoopThreadPool();

    EventLoopThreadPool(const EventLoopThreadPool&) = delete;
    EventLoopThreadPool& operator=(const EventLoopThreadPool&) = delete;

    void setThreadNum(int numThreads) { numThreads_ = numThreads; }
    // 启动全部线程并等待各 loop 就绪
    void start(const EventLoopThread::ThreadInitCallback& cb = EventLoopThread::ThreadInitCallback());
    // quit + join 全部线程
    void stop();

    // round-robin 取下一个 loop；只允许在 baseLoop 线程调用
    EventLoop* getNextLoop();
    std::vector<EventLoop*> getAllLoops();
    bool started() const { return started_; }
    const std::string& name() const { return name_; }

private:
    EventLoop* baseLoop_;
    std::string name_;
    bool started_ = false;
    int numThreads_ = 0;
    size_t next_ = 0;
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
};

} // namespace hvnetpp
