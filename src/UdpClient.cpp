#include "hvnetpp/UdpClient.h"
#include "UdpIo.h"
#include "hvnetpp/EventLoop.h"
#include "hvnetpp/EventLoopThread.h"
#include "hvnetpp/SocketsOps.h"
#include "rtclog.h"

#include <arpa/inet.h>
#include <assert.h>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <vector>

namespace hvnetpp {

UdpClient::UdpClient(std::string name)
    : name_(std::move(name)),
      ownThread_(new EventLoopThread(EventLoopThread::ThreadInitCallback(), name_ + "-loop")) {
}

UdpClient::UdpClient(EventLoop* loop, std::string name)
    : name_(std::move(name)),
      loop_(loop) {
}

UdpClient::~UdpClient() {
    stop();
    if (fd_ >= 0) {
        sockets::close(fd_);
    }
}

void UdpClient::setSocketOptions(const UdpSocketOptions& opts) {
    options_ = opts;
    if (fd_ >= 0) {
        udpio::applyOptions(fd_, remoteAddr_.family(), opts);
    }
}

int UdpClient::createsocket(int port, const char* host) {
    assert(!started_);
    if (host == nullptr) {
        host = "127.0.0.1";
    }
    struct in_addr addr4;
    struct in6_addr addr6;
    if (::inet_pton(AF_INET, host, &addr4) == 1) {
        remoteAddr_ = InetAddress(host, static_cast<uint16_t>(port), false);
    } else if (::inet_pton(AF_INET6, host, &addr6) == 1) {
        remoteAddr_ = InetAddress(host, static_cast<uint16_t>(port), true);
    } else {
        InetAddress resolved(static_cast<uint16_t>(port));
        if (!InetAddress::resolve(host, &resolved)) {
            RTCLOG(RTC_ERROR, "UdpClient[%s] resolve %s failed", name_.c_str(), host);
            return -1;
        }
        remoteAddr_ = resolved;
    }
    hasRemoteAddr_ = true;

    closesocket();
    fd_ = sockets::createNonblockingUdp(remoteAddr_.family());
    if (fd_ < 0) {
        RTCLOG(RTC_ERROR, "UdpClient[%s] 创建 socket 失败: %s", name_.c_str(), strerror(errno));
        return -1;
    }
    udpio::applyOptions(fd_, remoteAddr_.family(), options_);
    return fd_;
}

void UdpClient::closesocket() {
    assert(!started_);
    if (fd_ >= 0) {
        sockets::close(fd_);
        fd_ = -1;
    }
    connected_ = false;
}

int UdpClient::start(bool wait_threads_started) {
    if (!hasRemoteAddr_ || fd_ < 0) {
        RTCLOG(RTC_ERROR, "UdpClient[%s] start without createsocket", name_.c_str());
        return -1;
    }
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return 0;
    }

    if (connectedUdp_ && !connected_) {
        if (::connect(fd_, remoteAddr_.getSockAddr(), remoteAddr_.socklen()) < 0) {
            RTCLOG(RTC_ERROR, "UdpClient[%s] connect %s failed: %s",
                   name_.c_str(), remoteAddr_.toIpPort().c_str(), strerror(errno));
        } else {
            connected_ = true;
        }
    }

    if (ownThread_) {
        loop_ = ownThread_->startLoop();
    }
    auto setup = [this] {
        if (!started_.load(std::memory_order_acquire)) {
            return;   // start(false) 之后、本任务执行之前已在 loop 线程内 stop()
        }
        socket_.reset(new UdpSocket(loop_, name_, fd_, remoteAddr_.family(), false));
        socket_->setMessageCallback(onMessage);
        socket_->startReading();
    };
    if (wait_threads_started) {
        loop_->runInLoopAndWait(setup);
    } else {
        loop_->runInLoop(setup);
    }
    return 0;
}

void UdpClient::stop(bool wait_threads_stopped) {
    (void)wait_threads_stopped;
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false)) {
        return;
    }
    assert(!(ownThread_ && loop_->isInLoopThread()) && "UdpClient::stop() 不可在本客户端的回调里调用");

    if (!loop_->runInLoopAndWait([this] { socket_.reset(); })) {
        RTCLOG(RTC_ERROR, "UdpClient[%s] 所在 loop 已退出，请先 stop() 客户端再停止外部 loop", name_.c_str());
        socket_.release();
    }
    if (ownThread_) {
        ownThread_->stop();
        loop_ = nullptr;
    }
    RTCLOG(RTC_INFO, "UdpClient[%s] stopped", name_.c_str());
}

int UdpClient::send(const void* data, size_t len) {
    if (fd_ < 0) {
        errno = ENOTCONN;
        return -1;
    }
    const sa_family_t family = remoteAddr_.family();
    if (connected_.load(std::memory_order_acquire)) {
        return static_cast<int>(udpio::sendTo(fd_, family, data, len, nullptr, nullptr));
    }
    return static_cast<int>(udpio::sendTo(fd_, family, data, len, &remoteAddr_, nullptr));
}

int UdpClient::sendto(const void* data, size_t len, const InetAddress& peer) {
    if (fd_ < 0) {
        errno = ENOTCONN;
        return -1;
    }
    return static_cast<int>(udpio::sendTo(fd_, remoteAddr_.family(), data, len, &peer, nullptr));
}

int UdpClient::sendBatch(const UdpSendItem* items, size_t n) {
    if (fd_ < 0) {
        errno = ENOTCONN;
        return -1;
    }
    if (connected_.load(std::memory_order_acquire)) {
        return udpio::sendBatch(fd_, remoteAddr_.family(), items, n);
    }
    // 未 connect：peer 为空的项补上 remote 地址
    bool needFill = false;
    for (size_t i = 0; i < n && !needFill; ++i) {
        needFill = items[i].peer == nullptr;
    }
    if (!needFill) {
        return udpio::sendBatch(fd_, remoteAddr_.family(), items, n);
    }
    std::vector<UdpSendItem> filled(items, items + n);
    for (auto& item : filled) {
        if (item.peer == nullptr) {
            item.peer = &remoteAddr_;
        }
    }
    return udpio::sendBatch(fd_, remoteAddr_.family(), filled.data(), n);
}

} // namespace hvnetpp
