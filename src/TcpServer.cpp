#include "hvnetpp/TcpServer.h"
#include "hvnetpp/EventLoop.h"
#include "hvnetpp/Channel.h"
#include "hvnetpp/SocketsOps.h"
#include "hvnetpp/InetAddress.h"
#include "rtclog.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <cerrno>
#include <cstring>
#include <cstdio>

namespace hvnetpp {

namespace {

void cleanupAcceptor(EventLoop* loop, std::shared_ptr<Channel> channel, int acceptSocket, int idleFd) {
    auto cleanup = [loop, channel, acceptSocket, idleFd]() {
        if (channel) {
            channel->disableAll();
            channel->remove();
            loop->queueInLoop([channel]() {});
        }
        if (acceptSocket >= 0) {
            ::close(acceptSocket);
        }
        if (idleFd >= 0) {
            ::close(idleFd);
        }
    };

    if (loop->isInLoopThread()) {
        cleanup();
    } else {
        loop->queueInLoop(std::move(cleanup));
    }
}

int openIdleFd() {
    int idleFd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (idleFd < 0) {
        RTCLOG(RTC_ERROR, "Acceptor::openIdleFd error: %s", strerror(errno));
    }
    return idleFd;
}

} // namespace

// Internal Acceptor class
class Acceptor : public std::enable_shared_from_this<Acceptor> {
public:
    using NewConnectionCallback = std::function<void(int sockfd, const InetAddress&)>;

    Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reuseport)
        : loop_(loop),
          listenAddr_(listenAddr),
          reuseport_(reuseport),
          acceptSocket_(-1),
          acceptChannel_(),
          listening_(false),
          idleFd_(openIdleFd()) {
    }

    ~Acceptor() {
        std::shared_ptr<Channel> acceptChannel = std::move(acceptChannel_);
        const int acceptSocket = acceptSocket_;
        const int idleFd = idleFd_;
        acceptSocket_ = -1;
        idleFd_ = -1;
        cleanupAcceptor(loop_, std::move(acceptChannel), acceptSocket, idleFd);
    }

    void listen() {
        loop_->assertInLoopThread();
        if (listening_) {
            return;
        }

        const int sockfd = sockets::createNonblocking(listenAddr_.family());
        if (sockfd < 0) {
            return;
        }

        sockets::setReuseAddr(sockfd, true);
        sockets::setReusePort(sockfd, reuseport_);
        if (!sockets::bind(sockfd, listenAddr_.getSockAddr())) {
            sockets::close(sockfd);
            return;
        }
        if (!sockets::listen(sockfd)) {
            sockets::close(sockfd);
            return;
        }

        acceptSocket_ = sockfd;
        acceptChannel_ = std::make_shared<Channel>(loop_, acceptSocket_);
        acceptChannel_->setReadCallback(std::bind(&Acceptor::handleRead, this));
        acceptChannel_->tie(shared_from_this());
        acceptChannel_->enableReading();
        listening_ = true;
    }

    void setNewConnectionCallback(const NewConnectionCallback& cb) {
        newConnectionCallback_ = cb;
    }

private:
    void handleRead() {
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
                if (idleFd_ >= 0) {
                    ::close(idleFd_);
                }
                idleFd_ = ::accept(acceptSocket_, NULL, NULL);
                if (idleFd_ >= 0) {
                    ::close(idleFd_);
                }
                idleFd_ = openIdleFd();
            }
            break;
        }
    }

    EventLoop* loop_;
    InetAddress listenAddr_;
    bool reuseport_;
    int acceptSocket_;
    std::shared_ptr<Channel> acceptChannel_;
    NewConnectionCallback newConnectionCallback_;
    bool listening_;
    int idleFd_;
};

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listenAddr, const std::string& nameArg)
    : loop_(loop),
      name_(nameArg),
      acceptor_(std::make_shared<Acceptor>(loop, listenAddr, true)),
      callbackToken_(std::make_shared<bool>(true)),
      nextConnId_(1) {
    std::weak_ptr<bool> token = callbackToken_;
    acceptor_->setNewConnectionCallback([this, token](int sockfd, const InetAddress& peerAddr) {
        if (token.lock()) {
            newConnection(sockfd, peerAddr);
        } else {
            sockets::close(sockfd);
        }
    });
}

TcpServer::~TcpServer() {
    callbackToken_.reset();

    ConnectionMap connections = std::move(connections_);
    acceptor_.reset();
    for (auto& item : connections) {
        TcpConnectionPtr conn = std::move(item.second);
        if (!conn) {
            continue;
        }
        conn->setCloseCallback(TcpConnection::CloseCallback());
        conn->getLoop()->runInLoop([conn]() {
            conn->connectDestroyed();
        });
    }
}

void TcpServer::start() {
    std::shared_ptr<Acceptor> acceptor = acceptor_;
    std::weak_ptr<bool> token = callbackToken_;
    loop_->runInLoop([acceptor, token]() {
        if (token.lock()) {
            acceptor->listen();
        }
    });
}

void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr) {
    loop_->assertInLoopThread();
    char buf[64];
    snprintf(buf, sizeof buf, "-%s#%d", name_.c_str(), nextConnId_);
    ++nextConnId_;
    std::string connName = name_ + buf;

    struct sockaddr_in6 local = sockets::getLocalAddr(sockfd);
    InetAddress localAddr(local);

    TcpConnectionPtr conn(new TcpConnection(loop_, connName, sockfd, localAddr, peerAddr));
    connections_[connName] = conn;
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);
    std::weak_ptr<bool> token = callbackToken_;
    conn->setCloseCallback([this, token](const TcpConnectionPtr& connection) {
        if (token.lock()) {
            removeConnection(connection);
        }
    });
    
    loop_->runInLoop(std::bind(&TcpConnection::connectEstablished, conn));
}

void TcpServer::removeConnection(const TcpConnectionPtr& conn) {
    std::weak_ptr<bool> token = callbackToken_;
    loop_->runInLoop([this, token, conn]() {
        if (token.lock()) {
            removeConnectionInLoop(conn);
        }
    });
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn) {
    loop_->assertInLoopThread();
    size_t n = connections_.erase(conn->name());
    assert(n == 1);
    loop_->queueInLoop(std::bind(&TcpConnection::connectDestroyed, conn));
}

} // namespace hvnetpp
