#pragma once

#include "hvnetpp/EventLoopThread.h"

#include <memory>
#include <string>
#include <vector>

namespace hvnetpp {

class EventLoop;

// EventLoop 线程池；numThreads == 0 时所有接口退化为 baseLoop。
// 可以在多个 TcpServer / UdpServer / UdpSocket 之间共享（setEventLoopThreadPool），
// 让同一会话的信令、媒体、定时器落在同一个 loop 线程里。
class EventLoopThreadPool {
public:
    // baseLoop 可以为 nullptr（此时 numThreads 必须 >= 1）
    EventLoopThreadPool(EventLoop* baseLoop, std::string name);
    ~EventLoopThreadPool();

    EventLoopThreadPool(const EventLoopThreadPool&) = delete;
    EventLoopThreadPool& operator=(const EventLoopThreadPool&) = delete;

    void setThreadNum(int numThreads) { numThreads_ = numThreads; }
    // start() 前调用：第 i 个线程绑定到 cpus[i % cpus.size()]
    void setCpuAffinity(std::vector<int> cpus) { cpus_ = std::move(cpus); }
    // 启动全部线程并等待各 loop 就绪
    void start(const EventLoopThread::ThreadInitCallback& cb = EventLoopThread::ThreadInitCallback());
    // quit + join 全部线程
    void stop();

    // round-robin 取下一个 loop；只允许在 baseLoop 线程调用（baseLoop 为空时调用方自行保证串行）
    EventLoop* getNextLoop();
    // 全部 worker loop；没有 worker 时返回 {baseLoop}
    std::vector<EventLoop*> getAllLoops() const;
    // worker 数量（没有 worker 时为 1，即 baseLoop）与按下标访问，便于应用把会话哈希到固定 loop
    size_t size() const { return loops_.empty() ? 1 : loops_.size(); }
    EventLoop* loop(size_t idx) const { return loops_.empty() ? baseLoop_ : loops_[idx % loops_.size()]; }
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
    std::vector<int> cpus_;
};

} // namespace hvnetpp
