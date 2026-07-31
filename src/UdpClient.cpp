#include "hvnetpp/UdpClient.h"
#include "hvnetpp/Buffer.h"
#include "hvnetpp/EventLoop.h"
#include "hvnetpp/SocketsOps.h"
#include "rtclog.h"

#include <arpa/inet.h>
#include <assert.h>
#include <cerrno>
#include <cstring>
#include <future>
#include <sys/socket.h>

namespace hvnetpp {

UdpClient::UdpClient(std::string name)
    : name_(std::move(name)),
      loopThread_(EventLoopThread::ThreadInitCallback(), name_ + "-loop") {
}

UdpClient::~UdpClient() {
    if (started_) {
        stop();
    }
    if (fd_ >= 0) {
        sockets::close(fd_);
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

    if (fd_ >= 0) {
        sockets::close(fd_);
    }
    fd_ = sockets::createNonblockingUdpOrDie(remoteAddr_.family());
    return fd_;
}

void UdpClient::start(bool wait_threads_started) {
    (void)wait_threads_started;
    if (!hasRemoteAddr_ || fd_ < 0) {
        RTCLOG(RTC_ERROR, "UdpClient[%s] start without createsocket", name_.c_str());
        return;
    }
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return;
    }

    if (connectedUdp_) {
        socklen_t addrlen = static_cast<socklen_t>(
            remoteAddr_.family() == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
        if (::connect(fd_, remoteAddr_.getSockAddr(), addrlen) < 0) {
            RTCLOG(RTC_ERROR, "UdpClient[%s] connect %s failed: %s",
                   name_.c_str(), remoteAddr_.toIpPort().c_str(), strerror(errno));
        }
    }

    loop_ = loopThread_.startLoop();
    std::promise<void> ready;
    loop_->runInLoop([this, &ready] {
        socket_.reset(new UdpSocket(loop_, name_, fd_, remoteAddr_.family()));
        socket_->setReadCallback([this](const InetAddress& peer, Buffer* buf) {
            if (onMessage) onMessage(peer, buf);
        });
        socket_->startReading();
        ready.set_value();
    });
    ready.get_future().wait();
}

void UdpClient::stop(bool wait_threads_stopped) {
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
    RTCLOG(RTC_INFO, "UdpClient[%s] stopped", name_.c_str());
}

int UdpClient::send(const void* data, size_t len) {
    if (fd_ < 0) {
        return -1;
    }
    if (connectedUdp_) {
        return static_cast<int>(::send(fd_, data, len, 0));
    }
    return sendto(data, len, remoteAddr_);
}

int UdpClient::send(const std::string& msg) {
    return send(msg.data(), msg.size());
}

int UdpClient::sendto(const void* data, size_t len, const InetAddress& peer) {
    if (fd_ < 0) {
        return -1;
    }
    socklen_t addrlen = static_cast<socklen_t>(
        peer.family() == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
    ssize_t n = ::sendto(fd_, data, len, 0, peer.getSockAddr(), addrlen);
    return static_cast<int>(n);
}

} // namespace hvnetpp
