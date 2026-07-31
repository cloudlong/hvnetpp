#include "Acceptor.h"
#include "hvnetpp/Channel.h"
#include "hvnetpp/EventLoop.h"
#include "hvnetpp/SocketsOps.h"

#include <assert.h>
#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <unistd.h>

namespace hvnetpp {

Acceptor::Acceptor(EventLoop* loop, int listenfd)
    : loop_(loop),
      acceptSocket_(listenfd),
      acceptChannel_(std::make_shared<Channel>(loop, listenfd)),
      listening_(false),
      idleFd_(::open("/dev/null", O_RDONLY | O_CLOEXEC)) {
    assert(idleFd_ >= 0);
    acceptChannel_->setReadCallback(std::bind(&Acceptor::handleRead, this));
}

Acceptor::~Acceptor() {
    std::shared_ptr<Channel> acceptChannel = std::move(acceptChannel_);
    if (acceptChannel) {
        acceptChannel->disableAll();
        acceptChannel->remove();
        loop_->queueInLoop([acceptChannel]() {});
    }
    ::close(acceptSocket_);
    ::close(idleFd_);
}

void Acceptor::tieChannel() {
    acceptChannel_->tie(shared_from_this());
}

void Acceptor::listen() {
    loop_->assertInLoopThread();
    listening_ = true;
    sockets::listenOrDie(acceptSocket_);
    acceptChannel_->enableReading();
}

void Acceptor::handleRead() {
    loop_->assertInLoopThread();
    while (true) {
        struct sockaddr_in6 peerAddr;
        int connfd = sockets::accept(acceptSocket_, &peerAddr);
        if (connfd >= 0) {
            if (newConnectionCallback_) {
                InetAddress peer(peerAddr);
                newConnectionCallback_(connfd, peer);
            } else {
                sockets::close(connfd);
            }
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        if (errno == EMFILE) {
            ::close(idleFd_);
            idleFd_ = ::accept(acceptSocket_, NULL, NULL);
            ::close(idleFd_);
            idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        }
        break;
    }
}

} // namespace hvnetpp
