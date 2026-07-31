#include "hvnetpp/EventLoopThread.h"
#include "hvnetpp/EventLoop.h"

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
    EventLoop* loop = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        exiting_ = true;
        loop = loop_;
    }
    if (loop) {
        loop->quit();
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
