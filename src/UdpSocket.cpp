#include "hvnetpp/UdpSocket.h"
#include "hvnetpp/EventLoop.h"
#include "hvnetpp/Channel.h"
#include "hvnetpp/Buffer.h"
#include "hvnetpp/SocketsOps.h"
#include "rtclog.h"

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>

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
            sockets::close(sockfd);
        }
    };

    if (loop->isInLoopThread()) {
        cleanup();
    } else {
        loop->queueInLoop(std::move(cleanup));
    }
}

socklen_t socketAddrLength(const InetAddress& addr) {
    return static_cast<socklen_t>(
        addr.family() == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
}

} // namespace

UdpSocket::UdpSocket(EventLoop* loop, const std::string& name)
    : loop_(loop),
      name_(name),
      family_(AF_UNSPEC),
      sockfd_(-1),
      channel_(),
      callbackToken_(std::make_shared<bool>(true)),
      readBuf_(65536) { // Max UDP packet size
}

UdpSocket::~UdpSocket() {
    callbackToken_.reset();
    std::shared_ptr<Channel> channel = std::move(channel_);
    const int sockfd = sockfd_;
    sockfd_ = -1;
    cleanupUdpSocket(loop_, std::move(channel), sockfd);
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

    sockfd_ = sockets::createNonblockingUdp(family);
    if (sockfd_ < 0) {
        return false;
    }
    family_ = family;
    channel_ = std::make_shared<Channel>(loop_, sockfd_);

    sockets::setReuseAddr(sockfd_, true);
    sockets::setReusePort(sockfd_, true);
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
    if (!sockets::bind(sockfd_, addr.getSockAddr())) {
        if (createdSocket) {
            std::shared_ptr<Channel> channel = std::move(channel_);
            const int sockfd = sockfd_;
            family_ = AF_UNSPEC;
            sockfd_ = -1;
            cleanupUdpSocket(loop_, std::move(channel), sockfd);
        }
        return false;
    }
    std::shared_ptr<Channel> channel = channel_;
    loop_->runInLoop([channel]() {
        channel->enableReading();
    });
    return true;
}

ssize_t UdpSocket::sendTo(const void* data, size_t len, const InetAddress& destAddr) {
    if (!ensureSocket(destAddr.family())) {
        return -1;
    }
    return ::sendto(sockfd_, data, len, 0, destAddr.getSockAddr(), socketAddrLength(destAddr));
}

ssize_t UdpSocket::sendTo(Buffer* buf, const InetAddress& destAddr) {
    std::string payload = buf->retrieveAllAsString();
    return sendTo(payload.data(), payload.size(), destAddr);
}

void UdpSocket::handleRead() {
    loop_->assertInLoopThread();
    struct sockaddr_storage peerAddrStorage;
    socklen_t addrLen = sizeof peerAddrStorage;
    ssize_t n = ::recvfrom(sockfd_, readBuf_.data(), readBuf_.size(), 0, 
                           reinterpret_cast<struct sockaddr*>(&peerAddrStorage), &addrLen);
    
    if (n >= 0) {
        if (readCallback_) {
            inputBuffer_.retrieveAll();
            inputBuffer_.append(readBuf_.data(), n);
            if (peerAddrStorage.ss_family == AF_INET6) {
                const struct sockaddr_in6* peerAddr =
                    reinterpret_cast<const struct sockaddr_in6*>(&peerAddrStorage);
                InetAddress peer(*peerAddr);
                readCallback_(peer, &inputBuffer_);
            } else {
                const struct sockaddr_in* peerAddr =
                    reinterpret_cast<const struct sockaddr_in*>(&peerAddrStorage);
                InetAddress peer(*peerAddr);
                readCallback_(peer, &inputBuffer_);
            }
        }
    } else {
        RTCLOG(RTC_ERROR, "UdpSocket::handleRead() error: %s", strerror(errno));
    }
}

} // namespace hvnetpp
