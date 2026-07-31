#include "Connector.h"
#include "hvnetpp/Channel.h"
#include "hvnetpp/EventLoop.h"
#include "hvnetpp/SocketsOps.h"
#include "rtclog.h"

#include <cerrno>
#include <cstring>

namespace hvnetpp {

Connector::Connector(EventLoop* loop, const InetAddress& serverAddr)
    : loop_(loop),
      serverAddr_(serverAddr) {
}

Connector::~Connector() {
    // stopConnect 必须先在 loop 线程内被调用；此处仅兜底断言
    if (channel_) {
        RTCLOG(RTC_WARN, "Connector destructs with pending attempt");
    }
}

void Connector::startConnect() {
    loop_->assertInLoopThread();
    if (state_ == kConnecting) {
        return;
    }
    int sockfd = sockets::createNonblockingOrDie(serverAddr_.family());
    int ret = sockets::connect(sockfd, serverAddr_.getSockAddr());
    int savedErrno = (ret == 0) ? 0 : errno;
    switch (savedErrno) {
    case 0:
    case EINPROGRESS:
    case EINTR:
    case EISCONN:
        connecting(sockfd);
        break;
    default:
        RTCLOG(RTC_ERROR, "Connector connect to %s failed immediately: %s",
               serverAddr_.toIpPort().c_str(), strerror(savedErrno));
        sockets::close(sockfd);
        reportFailure();
        break;
    }
}

void Connector::connecting(int sockfd) {
    state_ = kConnecting;
    channel_ = std::make_shared<Channel>(loop_, sockfd);
    channel_->setWriteCallback(std::bind(&Connector::handleWrite, this));
    channel_->setErrorCallback(std::bind(&Connector::handleError, this));
    channel_->enableWriting();

    if (connectTimeoutMs_ > 0) {
        std::weak_ptr<Connector> weakSelf(shared_from_this());
        timeoutTimer_ = loop_->runAfter(connectTimeoutMs_ / 1000.0, [weakSelf] {
            std::shared_ptr<Connector> self = weakSelf.lock();
            if (self) {
                self->onConnectTimeout();
            }
        });
        hasTimeoutTimer_ = true;
    }
}

void Connector::handleWrite() {
    loop_->assertInLoopThread();
    if (state_ != kConnecting) {
        return;
    }
    state_ = kIdle;
    cancelTimeoutTimer();
    int sockfd = removeAndResetChannel();
    int err = sockets::getSocketError(sockfd);
    if (err != 0) {
        RTCLOG(RTC_WARN, "Connector connect to %s error: %s",
               serverAddr_.toIpPort().c_str(), strerror(err));
        sockets::close(sockfd);
        reportFailure();
    } else if (sockets::isSelfConnect(sockfd)) {
        RTCLOG(RTC_WARN, "Connector self connect to %s", serverAddr_.toIpPort().c_str());
        sockets::close(sockfd);
        reportFailure();
    } else {
        if (newConnectionCallback_) {
            newConnectionCallback_(sockfd);
        } else {
            sockets::close(sockfd);
        }
    }
}

void Connector::handleError() {
    loop_->assertInLoopThread();
    if (state_ != kConnecting) {
        return;
    }
    state_ = kIdle;
    cancelTimeoutTimer();
    int sockfd = removeAndResetChannel();
    int err = sockets::getSocketError(sockfd);
    RTCLOG(RTC_WARN, "Connector connect to %s poll error: %s",
           serverAddr_.toIpPort().c_str(), strerror(err));
    sockets::close(sockfd);
    reportFailure();
}

void Connector::onConnectTimeout() {
    loop_->assertInLoopThread();
    hasTimeoutTimer_ = false;
    if (state_ != kConnecting) {
        return;
    }
    state_ = kIdle;
    int sockfd = removeAndResetChannel();
    RTCLOG(RTC_WARN, "Connector connect to %s timeout (%d ms)",
           serverAddr_.toIpPort().c_str(), connectTimeoutMs_);
    sockets::close(sockfd);
    reportFailure();
}

void Connector::stopConnect() {
    loop_->assertInLoopThread();
    cancelTimeoutTimer();
    if (state_ == kConnecting) {
        state_ = kIdle;
        int sockfd = removeAndResetChannel();
        sockets::close(sockfd);
    }
}

int Connector::removeAndResetChannel() {
    int sockfd = channel_->fd();
    channel_->disableAll();
    channel_->remove();
    // 正处于 Channel 自身的回调中，延迟到下一轮再析构
    std::shared_ptr<Channel> channel = std::move(channel_);
    loop_->queueInLoop([channel] {});
    return sockfd;
}

void Connector::cancelTimeoutTimer() {
    if (hasTimeoutTimer_) {
        loop_->cancel(timeoutTimer_);
        hasTimeoutTimer_ = false;
    }
}

void Connector::reportFailure() {
    if (connectFailedCallback_) {
        connectFailedCallback_();
    }
}

} // namespace hvnetpp
