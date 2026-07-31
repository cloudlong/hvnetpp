#include "hvnetpp/EventLoopThreadPool.h"
#include "hvnetpp/EventLoop.h"

#include <assert.h>
#include <cstdio>

namespace hvnetpp {

EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, std::string name)
    : baseLoop_(baseLoop),
      name_(std::move(name)) {
}

EventLoopThreadPool::~EventLoopThreadPool() {
    stop();
}

void EventLoopThreadPool::start(const EventLoopThread::ThreadInitCallback& cb) {
    assert(!started_);
    started_ = true;

    for (int i = 0; i < numThreads_; ++i) {
        char name[64];
        snprintf(name, sizeof name, "%s-loop%d", name_.c_str(), i);
        threads_.emplace_back(new EventLoopThread(cb, name));
        loops_.push_back(threads_.back()->startLoop());
    }
    if (numThreads_ == 0 && cb) {
        cb(baseLoop_);
    }
}

void EventLoopThreadPool::stop() {
    for (auto& thread : threads_) {
        thread->stop();
    }
    threads_.clear();
    loops_.clear();
    started_ = false;
}

EventLoop* EventLoopThreadPool::getNextLoop() {
    baseLoop_->assertInLoopThread();
    assert(started_);
    EventLoop* loop = baseLoop_;
    if (!loops_.empty()) {
        loop = loops_[next_];
        next_ = (next_ + 1) % loops_.size();
    }
    return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::getAllLoops() {
    if (loops_.empty()) {
        return {baseLoop_};
    }
    return loops_;
}

} // namespace hvnetpp
