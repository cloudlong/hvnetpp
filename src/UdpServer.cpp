#include "hvnetpp/UdpServer.h"
#include "UdpIo.h"
#include "hvnetpp/EventLoop.h"
#include "hvnetpp/EventLoopThreadPool.h"
#include "hvnetpp/SocketsOps.h"
#include "rtclog.h"

#include <assert.h>
#include <cerrno>
#include <cstring>

namespace hvnetpp {

UdpServer::UdpServer(std::string name)
    : name_(std::move(name)) {
}

UdpServer::UdpServer(EventLoop* loop, std::string name)
    : name_(std::move(name)),
      externalLoop_(loop) {
}

UdpServer::~UdpServer() {
    stop();
    closeAll();
}

void UdpServer::closeAll() {
    for (int fd : fds_) {
        sockets::close(fd);
    }
    fds_.clear();
    fdsReusePort_ = false;
}

int UdpServer::createsocket(int port, const char* host) {
    assert(!started_);
    closeAll();
    bool ipv6 = (host != nullptr && ::strchr(host, ':') != nullptr);
    bindAddr_ = InetAddress(host ? host : "0.0.0.0", static_cast<uint16_t>(port), ipv6);
    // 已知要多 socket 时提前开 SO_REUSEPORT，避免 start() 时重建
    const bool reusePort = options_.reusePort || externalPool_ != nullptr || threadNum_ > 1;
    int fd = udpio::createBound(bindAddr_, options_, reusePort);
    if (fd < 0) {
        RTCLOG(RTC_ERROR, "UdpServer[%s] bind %s 失败: %s", name_.c_str(), bindAddr_.toIpPort().c_str(), strerror(errno));
        bound_ = false;
        return -1;
    }
    fds_.push_back(fd);
    fdsReusePort_ = reusePort;
    bound_ = true;
    // 端口为 0 时取内核分配的实际端口，后续 reuseport 兄弟 socket 绑到同一端口
    bindAddr_ = InetAddress(sockets::getLocalAddr(fd));
    RTCLOG(RTC_INFO, "UdpServer[%s] bound on %s", name_.c_str(), bindAddr_.toIpPort().c_str());
    return fd;
}

void UdpServer::closesocket() {
    assert(!started_);
    closeAll();
    bound_ = false;
}

int UdpServer::start(bool wait_threads_started) {
    (void)wait_threads_started;   // 启动本身是同步的，参数为对齐 libhv API 保留
    if (!bound_) {
        RTCLOG(RTC_ERROR, "UdpServer[%s] start without createsocket", name_.c_str());
        return -1;
    }
    if (started_.load(std::memory_order_acquire)) {
        return 0;
    }

    std::vector<EventLoop*> loops;
    if (externalPool_) {
        loops = externalPool_->getAllLoops();
    } else if (externalLoop_) {
        loops.push_back(externalLoop_);
    } else {
        ownPool_.reset(new EventLoopThreadPool(nullptr, name_));
        ownPool_->setThreadNum(threadNum_ > 0 ? threadNum_ : 1);
        ownPool_->start();
        loops = ownPool_->getAllLoops();
    }
    if (loops.empty() || loops[0] == nullptr) {
        RTCLOG(RTC_ERROR, "UdpServer[%s] 没有可用的 EventLoop（外部线程池未 start？）", name_.c_str());
        ownPool_.reset();
        return -1;
    }

    // 准备与 loop 数量相同的 fd：多 socket 时必须全部以 SO_REUSEPORT 绑定同一端口
    const size_t n = loops.size();
    const bool needReusePort = n > 1 || options_.reusePort;
    if (needReusePort && !fdsReusePort_) {
        RTCLOG(RTC_INFO, "UdpServer[%s] 需要 %zu 个 socket，以 SO_REUSEPORT 重新绑定 %s",
               name_.c_str(), n, bindAddr_.toIpPort().c_str());
        closeAll();
        fdsReusePort_ = true;
    }
    while (fds_.size() < n) {
        int fd = udpio::createBound(bindAddr_, options_, needReusePort);
        if (fd < 0) {
            RTCLOG(RTC_ERROR, "UdpServer[%s] 第 %zu 个 socket bind %s 失败: %s",
                   name_.c_str(), fds_.size(), bindAddr_.toIpPort().c_str(), strerror(errno));
            if (ownPool_) {
                ownPool_->stop();
                ownPool_.reset();
            }
            return -1;
        }
        fds_.push_back(fd);
    }
    while (fds_.size() > n) {
        sockets::close(fds_.back());
        fds_.pop_back();
    }

    loops_ = loops;
    sockets_.clear();
    sockets_.resize(n);
    const sa_family_t family = bindAddr_.family();
    bool ok = true;
    for (size_t i = 0; i < n && ok; ++i) {
        ok = loops_[i]->runInLoopAndWait([this, i, family] {
            std::unique_ptr<UdpSocket> sock(new UdpSocket(loops_[i], name_, fds_[i], family, false));
            sock->setMessageCallback(onMessage);   // 直接拷贝用户回调，每包少一层间接调用
            sock->startReading();
            sockets_[i] = std::move(sock);
        });
    }
    started_.store(true, std::memory_order_release);
    if (!ok) {
        RTCLOG(RTC_ERROR, "UdpServer[%s] 有 loop 已退出，启动失败", name_.c_str());
        stop();
        return -1;
    }
    RTCLOG(RTC_INFO, "UdpServer[%s] started on %s with %zu socket(s)", name_.c_str(), bindAddr_.toIpPort().c_str(), n);
    return 0;
}

void UdpServer::stop(bool wait_threads_stopped) {
    (void)wait_threads_stopped;
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false)) {
        return;
    }
    if (ownPool_) {
        // 自建线程里调用会在 join 自己时死锁
        EventLoop* current = EventLoop::getEventLoopOfCurrentThread();
        for (EventLoop* l : loops_) {
            assert(l != current && "UdpServer::stop() 不可在本服务器的回调里调用");
            (void)l;
            (void)current;
        }
    }
    for (size_t i = 0; i < sockets_.size(); ++i) {
        if (!sockets_[i]) {
            continue;
        }
        if (!loops_[i]->runInLoopAndWait([this, i] { sockets_[i].reset(); })) {
            // loop 已退出（外部线程池先于本对象停止）：不能再在别的线程注销 Channel，只能放弃这个对象
            RTCLOG(RTC_ERROR, "UdpServer[%s] socket %zu 所在 loop 已退出，请先 stop() 服务器再停止外部 loop",
                   name_.c_str(), i);
            sockets_[i].release();
        }
    }
    sockets_.clear();
    if (ownPool_) {
        ownPool_->stop();
        ownPool_.reset();
    }
    RTCLOG(RTC_INFO, "UdpServer[%s] stopped", name_.c_str());
}

int UdpServer::pickFd() const {
    if (fds_.size() > 1) {
        EventLoop* current = EventLoop::getEventLoopOfCurrentThread();
        if (current) {
            for (size_t i = 0; i < loops_.size(); ++i) {
                if (loops_[i] == current) {
                    return fds_[i];
                }
            }
        }
    }
    return fds_[0];
}

int UdpServer::sendto(const void* data, size_t len, const InetAddress& peer, const InetAddress* local) {
    if (!started_.load(std::memory_order_acquire)) {
        errno = ENOTCONN;
        return -1;
    }
    return static_cast<int>(udpio::sendTo(pickFd(), bindAddr_.family(), data, len, &peer, local));
}

int UdpServer::sendBatch(const UdpSendItem* items, size_t n) {
    if (!started_.load(std::memory_order_acquire)) {
        errno = ENOTCONN;
        return -1;
    }
    return udpio::sendBatch(pickFd(), bindAddr_.family(), items, n);
}

EventLoop* UdpServer::loop(size_t idx) const {
    if (idx < loops_.size() && started_.load(std::memory_order_acquire)) {
        return loops_[idx];
    }
    return idx == 0 ? externalLoop_ : nullptr;
}

} // namespace hvnetpp
