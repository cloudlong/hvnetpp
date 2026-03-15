#include "hvnetpp/UdpSocket.h"
#include "hvnetpp/EventLoop.h"
#include "hvnetpp/Channel.h"
#include "SocketsOps.h"
#include "rtclog.h"

#include <errno.h>
#include <cstring>
#include <sys/socket.h>

namespace hvnetpp {

namespace {

void cleanupUdpSocket(EventLoop* loop, std::shared_ptr<Channel> channel, int sockfd) {
    auto cleanup = [loop, channel, sockfd]() {
        if (channel) {
            channel->disableAll();
            channel->remove();
            loop->queueInLoop([channel]() {});
        }
        if (sockfd >= 0) {
            detail::sockets::close(sockfd);
        }
    };

    if (loop->isInLoopThread()) {
        cleanup();
    } else {
        loop->queueInLoop(std::move(cleanup));
    }
}
} // namespace

UdpSocket::UdpSocket(EventLoop* loop, const std::string& name)
    : loop_(loop),
      name_(name),
      family_(AF_UNSPEC),
      sockfd_(-1),
      state_(State::kClosed),
      channel_(),
      callbackToken_(std::make_shared<bool>(true)),
      readBuf_(65536) { // Max UDP packet size
}

UdpSocket::~UdpSocket() {
    callbackToken_.reset();
    state_.store(State::kClosed, std::memory_order_release);
    std::shared_ptr<Channel> channel = std::move(channel_);
    const int sockfd = sockfd_;
    sockfd_ = -1;
    cleanupUdpSocket(loop_, std::move(channel), sockfd);
}

bool UdpSocket::open(sa_family_t family) {
    return ensureSocket(family);
}

bool UdpSocket::ensureSocket(sa_family_t family) {
    if (sockfd_ >= 0) {
        if (family_ != family) {
            RTCLOG(RTC_ERROR, "UdpSocket::ensureSocket() family mismatch: existing=%d requested=%d", family_, family);
            errno = EAFNOSUPPORT;
            return false;
        }
        return true;
    }

    sockfd_ = detail::sockets::createNonblockingUdp(family);
    if (sockfd_ < 0) {
        return false;
    }
    family_ = family;
    state_.store(State::kOpened, std::memory_order_release);
    channel_ = std::make_shared<Channel>(loop_, sockfd_);

    detail::sockets::setReuseAddr(sockfd_, true);
    detail::sockets::setReusePort(sockfd_, true);
    std::weak_ptr<bool> token = callbackToken_;
    channel_->setReadCallback([this, token]() {
        if (token.lock()) {
            handleRead();
        }
    });
    return true;
}

bool UdpSocket::bind(const InetAddress& addr) {
    const bool createdSocket = sockfd_ < 0;
    if (!ensureSocket(addr.family())) {
        return false;
    }
    if (!detail::sockets::bind(sockfd_, addr)) {
        if (createdSocket) {
            std::shared_ptr<Channel> channel = std::move(channel_);
            const int sockfd = sockfd_;
            family_ = AF_UNSPEC;
            sockfd_ = -1;
            state_.store(State::kClosed, std::memory_order_release);
            cleanupUdpSocket(loop_, std::move(channel), sockfd);
        }
        return false;
    }
    if (state() != State::kReceiving) {
        state_.store(State::kBound, std::memory_order_release);
    }
    return true;
}

bool UdpSocket::bindAndStart(const InetAddress& addr) {
    if (!bind(addr)) {
        return false;
    }
    return startReceive();
}

bool UdpSocket::startReceive() {
    std::shared_ptr<Channel> channel = channel_;
    if (!channel || !isBound()) {
        errno = EINVAL;
        RTCLOG(RTC_WARN, "UdpSocket::startReceive() requires a bound socket");
        return false;
    }
    if (state() == State::kReceiving) {
        return true;
    }
    state_.store(State::kReceiving, std::memory_order_release);
    loop_->runInLoop([channel]() {
        channel->enableReading();
    });
    return true;
}

void UdpSocket::stopReceive() {
    std::shared_ptr<Channel> channel = channel_;
    if (!channel) {
        return;
    }
    if (state() == State::kReceiving) {
        state_.store(State::kBound, std::memory_order_release);
    }
    loop_->runInLoop([channel]() {
        channel->disableReading();
    });
}

ssize_t UdpSocket::sendTo(const void* data, size_t len, const InetAddress& destAddr) {
    if (!ensureSocket(destAddr.family())) {
        return -1;
    }
    return ::sendto(sockfd_, data, len, 0, destAddr.getSockAddr(), destAddr.length());
}

ssize_t UdpSocket::sendTo(const Buffer& buf, const InetAddress& destAddr) {
    return sendTo(buf.peek(), buf.readableBytes(), destAddr);
}

void UdpSocket::handleRead() {
    loop_->assertInLoopThread();
    struct sockaddr_storage peerAddrStorage;
    socklen_t addrLen = sizeof peerAddrStorage;
    ssize_t n = ::recvfrom(sockfd_, readBuf_.data(), readBuf_.size(), 0, 
                           reinterpret_cast<struct sockaddr*>(&peerAddrStorage), &addrLen);
    
    if (n >= 0) {
        if (readCallback_) {
            InetAddress peer(reinterpret_cast<const struct sockaddr*>(&peerAddrStorage), addrLen);
            readCallback_(peer, readBuf_.data(), static_cast<size_t>(n));
        }
    } else {
        RTCLOG(RTC_ERROR, "UdpSocket::handleRead() error: %s", strerror(errno));
    }
}

} // namespace hvnetpp
