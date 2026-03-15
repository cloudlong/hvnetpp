#include "hvnetpp/TcpConnection.h"
#include "hvnetpp/Channel.h"
#include "hvnetpp/EventLoop.h"
#include "hvnetpp/SocketsOps.h"
#include "rtclog.h"
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <assert.h>

namespace hvnetpp {

TcpConnection::TcpConnection(EventLoop* loop,
                             const std::string& nameArg,
                             int sockfd,
                             const InetAddress& localAddr,
                             const InetAddress& peerAddr)
    : loop_(loop),
      name_(nameArg),
      state_(kConnecting),
      channel_(new Channel(loop, sockfd)),
      socketFd_(sockfd),
      localAddr_(localAddr),
      peerAddr_(peerAddr),
      highWaterMark_(64*1024*1024) {
    channel_->setReadCallback(std::bind(&TcpConnection::handleRead, this));
    channel_->setWriteCallback(std::bind(&TcpConnection::handleWrite, this));
    channel_->setCloseCallback(std::bind(&TcpConnection::handleClose, this));
    channel_->setErrorCallback(std::bind(static_cast<void (TcpConnection::*)()>(&TcpConnection::handleError), this));
}

TcpConnection::~TcpConnection() {
    assert(state() == kDisconnected);
    closeSocket();
}

void TcpConnection::connectEstablished() {
    loop_->assertInLoopThread();
    assert(state() == kConnecting);
    setState(kConnected);
    channel_->enableReading();
    if (connectionCallback_) {
        connectionCallback_(shared_from_this());
    }
}

void TcpConnection::connectDestroyed() {
    loop_->assertInLoopThread();
    if (state() != kDisconnected) {
        setState(kDisconnected);
        channel_->disableAll();
        if (connectionCallback_) {
            connectionCallback_(shared_from_this());
        }
    }
    channel_->remove();
    closeSocket();
}

void TcpConnection::handleRead() {
    loop_->assertInLoopThread();
    if (state() == kDisconnected) {
        return;
    }
    int savedErrno = 0;
    ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
    if (n > 0) {
        if (messageCallback_) {
            messageCallback_(shared_from_this(), &inputBuffer_);
        }
    } else if (n == 0) {
        handleClose();
    } else {
        handleError(savedErrno);
    }
}

void TcpConnection::handleWrite() {
    loop_->assertInLoopThread();
    if (state() == kDisconnected) {
        return;
    }
    if (channel_->isWriting()) {
        ssize_t n = ::write(channel_->fd(), outputBuffer_.peek(), outputBuffer_.readableBytes());
        if (n > 0) {
            outputBuffer_.retrieve(n);
            if (outputBuffer_.readableBytes() == 0) {
                channel_->disableWriting();
                if (writeCompleteCallback_) {
                    loop_->queueInLoop(std::bind(writeCompleteCallback_, shared_from_this()));
                }
                if (state() == kDisconnecting) {
                    shutdownInLoop();
                }
            }
        } else {
            handleError(n == 0 ? EPIPE : errno);
        }
    }
}

void TcpConnection::handleClose() {
    loop_->assertInLoopThread();
    assert(state() == kConnected || state() == kDisconnecting);
    setState(kDisconnected);
    channel_->disableAll();

    TcpConnectionPtr guardThis(shared_from_this());
    if (connectionCallback_) {
        connectionCallback_(guardThis);
    }
    if (closeCallback_) {
        closeCallback_(guardThis);
    }
}

void TcpConnection::handleError() {
    int err = sockets::getSocketError(channel_->fd());
    if (err != 0) {
        handleError(err);
    }
}

void TcpConnection::handleError(int err) {
    if (err == 0 || err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
        return;
    }
    RTCLOG(RTC_ERROR, "TcpConnection::handleError name=%s - error=%d: %s", name_.c_str(), err, strerror(err));
    if (state() == kConnected || state() == kDisconnecting) {
        handleClose();
    }
}

void TcpConnection::send(const std::string& message) {
    if (loop_->isInLoopThread()) {
        if (state() == kConnected) {
            sendInLoop(message);
        }
    } else if (state() == kConnected) {
        TcpConnectionPtr self(shared_from_this());
        loop_->queueInLoop([self, message]() {
            if (self->state() == kConnected) {
                self->sendInLoop(message);
            }
        });
    }
}

void TcpConnection::send(Buffer* buf) {
    if (loop_->isInLoopThread()) {
        if (state() == kConnected) {
            sendInLoop(buf->peek(), buf->readableBytes());
            buf->retrieveAll();
        }
    } else if (state() == kConnected) {
        std::string message = buf->retrieveAllAsString();
        TcpConnectionPtr self(shared_from_this());
        loop_->queueInLoop([self, message]() {
            if (self->state() == kConnected) {
                self->sendInLoop(message);
            }
        });
    }
}

void TcpConnection::sendInLoop(const std::string& message) {
    sendInLoop(message.data(), message.size());
}

void TcpConnection::sendInLoop(const void* data, size_t len) {
    loop_->assertInLoopThread();
    ssize_t nwrote = 0;
    size_t remaining = len;
    bool faultError = false;

    if (state() == kDisconnected) {
        return;
    }

    // if no thing in output queue, try write directly
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
        nwrote = ::write(channel_->fd(), data, len);
        if (nwrote >= 0) {
            remaining = len - nwrote;
            if (remaining == 0 && writeCompleteCallback_) {
                loop_->queueInLoop(std::bind(writeCompleteCallback_, shared_from_this()));
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
            loop_->queueInLoop(std::bind(highWaterMarkCallback_, shared_from_this(), oldLen + remaining));
        }
        outputBuffer_.append(static_cast<const char*>(data) + nwrote, remaining);
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
    }
}

void TcpConnection::shutdown() {
    if (loop_->isInLoopThread()) {
        if (state() == kConnected) {
            setState(kDisconnecting);
            shutdownInLoop();
        }
    } else if (state() == kConnected) {
        TcpConnectionPtr self(shared_from_this());
        loop_->queueInLoop([self]() {
            if (self->state() == kConnected) {
                self->setState(kDisconnecting);
                self->shutdownInLoop();
            }
        });
    }
}

void TcpConnection::shutdownInLoop() {
    loop_->assertInLoopThread();
    if (socketFd_ >= 0 && !channel_->isWriting()) {
        sockets::shutdownWrite(socketFd_);
    }
}

void TcpConnection::setTcpNoDelay(bool on) {
    if (socketFd_ >= 0) {
        sockets::setTcpNoDelay(socketFd_, on);
    }
}

void TcpConnection::closeSocket() {
    if (socketFd_ >= 0) {
        sockets::close(socketFd_);
        socketFd_ = -1;
    }
}

} // namespace hvnetpp
