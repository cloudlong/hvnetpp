#include "hvnetpp/SocketChannel.h"
#include "hvnetpp/Channel.h"
#include "hvnetpp/EventLoop.h"
#include "hvnetpp/SocketsOps.h"
#include "rtclog.h"

#include <assert.h>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace hvnetpp {

namespace {
std::atomic<uint32_t> s_channelId{0};
}

SocketChannel::SocketChannel(EventLoop* loop, int sockfd,
                             const InetAddress& localAddr, const InetAddress& peerAddr)
    : loop_(loop),
      id_(++s_channelId),
      status_(CONNECTING),
      channel_(new Channel(loop, sockfd)),
      sockfd_(sockfd),
      localAddr_(localAddr),
      peerAddr_(peerAddr),
      highWaterMark_(64 * 1024 * 1024),
      lastActivity_(std::chrono::steady_clock::now()) {
    channel_->setReadCallback(std::bind(&SocketChannel::handleRead, this));
    channel_->setWriteCallback(std::bind(&SocketChannel::handleWrite, this));
    channel_->setCloseCallback(std::bind(&SocketChannel::handleClose, this));
    channel_->setErrorCallback(std::bind(static_cast<void (SocketChannel::*)()>(&SocketChannel::handleError), this));
}

SocketChannel::~SocketChannel() {
    assert(status() == DISCONNECTED);
    closeSocket();
}

void SocketChannel::connectEstablished() {
    loop_->assertInLoopThread();
    assert(status() == CONNECTING);
    setStatus(CONNECTED);
    lastActivity_ = std::chrono::steady_clock::now();
    channel_->tie(shared_from_this());
    channel_->enableReading();
    if (keepaliveTimeoutMs_ > 0) {
        armKeepaliveTimer(keepaliveTimeoutMs_ / 1000.0);
    }
    if (heartbeatIntervalMs_ > 0) {
        armHeartbeatTimer();
    }
    if (connectionCallback_) {
        connectionCallback_(shared_from_this());
    }
}

void SocketChannel::connectDestroyed() {
    loop_->assertInLoopThread();
    if (destroyed_) {
        // 停机路径（forceCloseInLoop）与 closeCallback 排队的 connectDestroyed 可能双双到达
        return;
    }
    destroyed_ = true;
    cancelTimersInLoop();
    if (status() != DISCONNECTED) {
        setStatus(DISCONNECTED);
        channel_->disableAll();
        if (connectionCallback_) {
            connectionCallback_(shared_from_this());
        }
    }
    channel_->remove();
    closeSocket();
}

void SocketChannel::forceCloseInLoop() {
    loop_->assertInLoopThread();
    if (status() == CONNECTED || status() == DISCONNECTING) {
        handleClose();
    }
    connectDestroyed();
}

void SocketChannel::handleRead() {
    loop_->assertInLoopThread();
    if (status() == DISCONNECTED) {
        return;
    }
    int savedErrno = 0;
    ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
    if (n > 0) {
        lastActivity_ = std::chrono::steady_clock::now();
        if (unpack_) {
            handleUnpack();
        } else if (messageCallback_) {
            messageCallback_(shared_from_this(), &inputBuffer_);
        }
    } else if (n == 0) {
        handleClose();
    } else {
        handleError(savedErrno);
    }
}

void SocketChannel::handleUnpack() {
    while (status() == CONNECTED) {
        bool protoError = false;
        int r = unpackBuffer(*unpack_, &inputBuffer_, &packetBuffer_, &protoError);
        if (r == 1) {
            if (messageCallback_) {
                messageCallback_(shared_from_this(), &packetBuffer_);
            }
        } else if (r < 0) {
            RTCLOG(RTC_ERROR, "SocketChannel id=%u peer=%s unpack protocol error, closing",
                   id_, peeraddr().c_str());
            handleClose();
            return;
        } else {
            return; // 数据不足，等待下次读
        }
    }
}

void SocketChannel::handleWrite() {
    loop_->assertInLoopThread();
    if (status() == DISCONNECTED) {
        return;
    }
    if (channel_->isWriting()) {
        ssize_t n = ::write(channel_->fd(), outputBuffer_.peek(), outputBuffer_.readableBytes());
        if (n > 0) {
            lastActivity_ = std::chrono::steady_clock::now();
            outputBuffer_.retrieve(n);
            if (outputBuffer_.readableBytes() == 0) {
                channel_->disableWriting();
                if (writeCompleteCallback_) {
                    WriteCompleteCallback cb = writeCompleteCallback_;
                    SocketChannelPtr self(shared_from_this());
                    loop_->queueInLoop([cb, self] { cb(self); });
                }
                if (status() == DISCONNECTING) {
                    shutdownInLoop();
                }
            }
        } else {
            handleError(n == 0 ? EPIPE : errno);
        }
    }
}

void SocketChannel::handleClose() {
    loop_->assertInLoopThread();
    assert(status() == CONNECTED || status() == DISCONNECTING);
    setStatus(DISCONNECTED);
    channel_->disableAll();

    SocketChannelPtr guardThis(shared_from_this());
    if (connectionCallback_) {
        connectionCallback_(guardThis);
    }
    if (closeCallback_) {
        closeCallback_(guardThis);
    }
}

void SocketChannel::handleError() {
    int err = sockets::getSocketError(channel_->fd());
    if (err != 0) {
        handleError(err);
    }
}

void SocketChannel::handleError(int err) {
    if (err == 0 || err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
        return;
    }
    RTCLOG(RTC_ERROR, "SocketChannel id=%u peer=%s - error=%d: %s",
           id_, peeraddr().c_str(), err, strerror(err));
    if (status() == CONNECTED || status() == DISCONNECTING) {
        handleClose();
    }
}

int SocketChannel::write(const void* data, size_t len) {
    if (status() != CONNECTED) {
        return -1;
    }
    if (loop_->isInLoopThread()) {
        sendInLoop(data, len);
    } else {
        std::string message(static_cast<const char*>(data), len);
        SocketChannelPtr self(shared_from_this());
        loop_->queueInLoop([self, msg = std::move(message)] {
            if (self->status() == CONNECTED) {
                self->sendInLoop(msg);
            }
        });
    }
    return static_cast<int>(len);
}

int SocketChannel::write(const std::string& msg) {
    return write(msg.data(), msg.size());
}

int SocketChannel::write(Buffer* buf) {
    if (status() != CONNECTED) {
        return -1;
    }
    if (loop_->isInLoopThread()) {
        size_t len = buf->readableBytes();
        sendInLoop(buf->peek(), len);
        buf->retrieveAll();
        return static_cast<int>(len);
    }
    std::string message = buf->retrieveAllAsString();
    size_t len = message.size();
    SocketChannelPtr self(shared_from_this());
    loop_->queueInLoop([self, msg = std::move(message)] {
        if (self->status() == CONNECTED) {
            self->sendInLoop(msg);
        }
    });
    return static_cast<int>(len);
}

void SocketChannel::sendInLoop(const std::string& message) {
    sendInLoop(message.data(), message.size());
}

void SocketChannel::sendInLoop(const void* data, size_t len) {
    loop_->assertInLoopThread();
    ssize_t nwrote = 0;
    size_t remaining = len;
    bool faultError = false;

    if (status() == DISCONNECTED) {
        return;
    }

    // 输出缓冲为空则先尝试直接写
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
        nwrote = ::write(channel_->fd(), data, len);
        if (nwrote >= 0) {
            lastActivity_ = std::chrono::steady_clock::now();
            remaining = len - nwrote;
            if (remaining == 0 && writeCompleteCallback_) {
                WriteCompleteCallback cb = writeCompleteCallback_;
                SocketChannelPtr self(shared_from_this());
                loop_->queueInLoop([cb, self] { cb(self); });
            }
        } else {
            const int savedErrno = errno;
            nwrote = 0;
            if (savedErrno != EWOULDBLOCK && savedErrno != EAGAIN && savedErrno != EINTR) {
                faultError = true;
                handleError(savedErrno);
                remaining = 0;
            }
        }
    }

    if (!faultError && remaining > 0) {
        size_t oldLen = outputBuffer_.readableBytes();
        if (oldLen + remaining >= highWaterMark_
            && oldLen < highWaterMark_
            && highWaterMarkCallback_) {
            HighWaterMarkCallback cb = highWaterMarkCallback_;
            SocketChannelPtr self(shared_from_this());
            size_t queued = oldLen + remaining;
            loop_->queueInLoop([cb, self, queued] { cb(self, queued); });
        }
        outputBuffer_.append(static_cast<const char*>(data) + nwrote, remaining);
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
    }
}

void SocketChannel::close() {
    SocketChannelPtr self(shared_from_this());
    loop_->runInLoop([self] {
        if (self->status() == CONNECTED || self->status() == DISCONNECTING) {
            self->handleClose();
        }
    });
}

void SocketChannel::shutdown() {
    if (status() != CONNECTED) {
        return;
    }
    SocketChannelPtr self(shared_from_this());
    loop_->runInLoop([self] {
        if (self->status() == CONNECTED) {
            self->setStatus(DISCONNECTING);
            self->shutdownInLoop();
        }
    });
}

void SocketChannel::shutdownInLoop() {
    loop_->assertInLoopThread();
    if (sockfd_ >= 0 && !channel_->isWriting()) {
        sockets::shutdownWrite(sockfd_);
    }
}

void SocketChannel::setTcpNoDelay(bool on) {
    if (sockfd_ >= 0) {
        sockets::setTcpNoDelay(sockfd_, on);
    }
}

size_t SocketChannel::writeBufferSize() const {
    return outputBuffer_.readableBytes();
}

void SocketChannel::setUnpack(const unpack_setting_t* setting) {
    if (setting && setting->mode != UNPACK_MODE_NONE) {
        unpack_.reset(new unpack_setting_t(*setting));
    } else {
        unpack_.reset();
    }
}

void SocketChannel::setKeepaliveTimeout(int timeoutMs) {
    keepaliveTimeoutMs_ = timeoutMs;
    if (status() == CONNECTED && loop_->isInLoopThread()) {
        if (hasKeepaliveTimer_) {
            loop_->cancel(keepaliveTimer_);
            hasKeepaliveTimer_ = false;
        }
        if (timeoutMs > 0) {
            lastActivity_ = std::chrono::steady_clock::now();
            armKeepaliveTimer(timeoutMs / 1000.0);
        }
    }
}

void SocketChannel::setHeartbeat(int intervalMs, std::function<void()> fn) {
    heartbeatIntervalMs_ = intervalMs;
    heartbeatFn_ = std::move(fn);
    if (status() == CONNECTED && loop_->isInLoopThread()) {
        if (hasHeartbeatTimer_) {
            loop_->cancel(heartbeatTimer_);
            hasHeartbeatTimer_ = false;
        }
        if (intervalMs > 0 && heartbeatFn_) {
            armHeartbeatTimer();
        }
    }
}

void SocketChannel::armKeepaliveTimer(double delaySeconds) {
    loop_->assertInLoopThread();
    std::weak_ptr<SocketChannel> weakSelf(shared_from_this());
    keepaliveTimer_ = loop_->runAfter(delaySeconds, [weakSelf] {
        SocketChannelPtr self = weakSelf.lock();
        if (self) {
            self->onKeepaliveTimer();
        }
    });
    hasKeepaliveTimer_ = true;
}

void SocketChannel::onKeepaliveTimer() {
    loop_->assertInLoopThread();
    hasKeepaliveTimer_ = false;
    if (status() != CONNECTED || keepaliveTimeoutMs_ <= 0) {
        return;
    }
    using namespace std::chrono;
    const auto now = steady_clock::now();
    const auto elapsedMs = duration_cast<milliseconds>(now - lastActivity_).count();
    if (elapsedMs >= keepaliveTimeoutMs_) {
        RTCLOG(RTC_WARN, "SocketChannel id=%u peer=%s keepalive timeout (%lld ms >= %d ms), closing",
               id_, peeraddr().c_str(), static_cast<long long>(elapsedMs), keepaliveTimeoutMs_);
        handleClose();
    } else {
        // 未超时：按剩余时间重臂，避免每次 IO 都操作定时器
        armKeepaliveTimer((keepaliveTimeoutMs_ - elapsedMs) / 1000.0);
    }
}

void SocketChannel::armHeartbeatTimer() {
    loop_->assertInLoopThread();
    std::weak_ptr<SocketChannel> weakSelf(shared_from_this());
    heartbeatTimer_ = loop_->runEvery(heartbeatIntervalMs_ / 1000.0, [weakSelf] {
        SocketChannelPtr self = weakSelf.lock();
        if (self && self->status() == CONNECTED && self->heartbeatFn_) {
            self->heartbeatFn_();
        }
    });
    hasHeartbeatTimer_ = true;
}

void SocketChannel::cancelTimersInLoop() {
    loop_->assertInLoopThread();
    if (hasKeepaliveTimer_) {
        loop_->cancel(keepaliveTimer_);
        hasKeepaliveTimer_ = false;
    }
    if (hasHeartbeatTimer_) {
        loop_->cancel(heartbeatTimer_);
        hasHeartbeatTimer_ = false;
    }
}

void SocketChannel::closeSocket() {
    if (sockfd_ >= 0) {
        sockets::close(sockfd_);
        sockfd_ = -1;
    }
}

} // namespace hvnetpp
