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
      channel_(nullptr),
      readBuf_(65536) { // Max UDP packet size
}

UdpSocket::~UdpSocket() {
    if (channel_) {
        channel_->disableAll();
        channel_->remove();
    }
    if (sockfd_ >= 0) {
        sockets::close(sockfd_);
    }
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

    sockfd_ = sockets::createNonblockingUdpOrDie(family);
    family_ = family;
    channel_.reset(new Channel(loop_, sockfd_));

    sockets::setReuseAddr(sockfd_, true);
    sockets::setReusePort(sockfd_, true);
    channel_->setReadCallback(std::bind(&UdpSocket::handleRead, this));
    return true;
}

bool UdpSocket::bind(const InetAddress& addr) {
    if (!ensureSocket(addr.family())) {
        return false;
    }
    sockets::bindOrDie(sockfd_, addr.getSockAddr());
    channel_->enableReading();
    return true;
}

ssize_t UdpSocket::sendTo(const void* data, size_t len, const InetAddress& destAddr) {
    if (!ensureSocket(destAddr.family())) {
        return -1;
    }
    return ::sendto(sockfd_, data, len, 0, destAddr.getSockAddr(), socketAddrLength(destAddr));
}

ssize_t UdpSocket::sendTo(Buffer* buf, const InetAddress& destAddr) {
    return sendTo(buf->peek(), buf->readableBytes(), destAddr);
}

void UdpSocket::handleRead() {
    loop_->assertInLoopThread();
    struct sockaddr_storage peerAddrStorage;
    socklen_t addrLen = sizeof peerAddrStorage;
    ssize_t n = ::recvfrom(sockfd_, readBuf_.data(), readBuf_.size(), 0, 
                           reinterpret_cast<struct sockaddr*>(&peerAddrStorage), &addrLen);
    
    if (n >= 0) {
        if (readCallback_) {
            Buffer buf;
            buf.append(readBuf_.data(), n);
            if (peerAddrStorage.ss_family == AF_INET6) {
                const struct sockaddr_in6* peerAddr =
                    reinterpret_cast<const struct sockaddr_in6*>(&peerAddrStorage);
                InetAddress peer(*peerAddr);
                readCallback_(peer, &buf);
            } else {
                const struct sockaddr_in* peerAddr =
                    reinterpret_cast<const struct sockaddr_in*>(&peerAddrStorage);
                InetAddress peer(*peerAddr);
                readCallback_(peer, &buf);
            }
        }
    } else {
        RTCLOG(RTC_ERROR, "UdpSocket::handleRead() error: %s", strerror(errno));
    }
}

} // namespace hvnetpp
