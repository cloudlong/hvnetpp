#include "hvnetpp/UdpServer.h"
#include "hvnetpp/Buffer.h"
#include "hvnetpp/EventLoop.h"
#include "hvnetpp/SocketsOps.h"
#include "rtclog.h"

#include <assert.h>
#include <cerrno>
#include <cstring>
#include <future>
#include <sys/socket.h>

namespace hvnetpp {

UdpServer::UdpServer(std::string name)
    : name_(std::move(name)),
      loopThread_(EventLoopThread::ThreadInitCallback(), name_ + "-loop"),
      family_(AF_UNSPEC) {
}

UdpServer::~UdpServer() {
    if (started_) {
        stop();
    }
    if (fd_ >= 0) {
        sockets::close(fd_);
    }
}

int UdpServer::createsocket(int port, const char* host) {
    assert(!started_);
    if (fd_ >= 0) {
        sockets::close(fd_);
        fd_ = -1;
    }
    bool ipv6 = (host != nullptr && ::strchr(host, ':') != nullptr);
    InetAddress bindAddr(host ? host : "0.0.0.0", static_cast<uint16_t>(port), ipv6);
    int fd = sockets::createNonblockingUdpOrDie(bindAddr.family());
    sockets::setReuseAddr(fd, true);
    sockets::setReusePort(fd, true);
    socklen_t addrlen = ipv6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    if (::bind(fd, bindAddr.getSockAddr(), addrlen) < 0) {
        RTCLOG(RTC_ERROR, "UdpServer[%s] bind %s failed: %s",
               name_.c_str(), bindAddr.toIpPort().c_str(), strerror(errno));
        sockets::close(fd);
        return -1;
    }
    fd_ = fd;
    family_ = bindAddr.family();
    RTCLOG(RTC_INFO, "UdpServer[%s] bound on %s", name_.c_str(), bindAddr.toIpPort().c_str());
    return fd_;
}

void UdpServer::start(bool wait_threads_started) {
    (void)wait_threads_started;
    if (fd_ < 0) {
        RTCLOG(RTC_ERROR, "UdpServer[%s] start without createsocket", name_.c_str());
        return;
    }
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return;
    }

    loop_ = loopThread_.startLoop();
    std::promise<void> ready;
    loop_->runInLoop([this, &ready] {
        socket_.reset(new UdpSocket(loop_, name_, fd_, family_));
        socket_->setReadCallback([this](const InetAddress& peer, Buffer* buf) {
            if (onMessage) onMessage(peer, buf);
        });
        socket_->startReading();
        ready.set_value();
    });
    ready.get_future().wait();
}

void UdpServer::stop(bool wait_threads_stopped) {
    (void)wait_threads_stopped;
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false)) {
        return;
    }
    assert(loop_ != nullptr && !loop_->isInLoopThread());

    std::promise<void> done;
    loop_->runInLoop([this, &done] {
        socket_.reset();   // UdpSocket 析构会关闭 fd
        done.set_value();
    });
    done.get_future().wait();
    fd_ = -1;

    loopThread_.stop();
    loop_ = nullptr;
    RTCLOG(RTC_INFO, "UdpServer[%s] stopped", name_.c_str());
}

int UdpServer::sendto(const void* data, size_t len, const InetAddress& peer) {
    if (fd_ < 0) {
        return -1;
    }
    socklen_t addrlen = static_cast<socklen_t>(
        peer.family() == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
    ssize_t n = ::sendto(fd_, data, len, 0, peer.getSockAddr(), addrlen);
    return static_cast<int>(n);
}

int UdpServer::sendto(const std::string& msg, const InetAddress& peer) {
    return sendto(msg.data(), msg.size(), peer);
}

} // namespace hvnetpp
