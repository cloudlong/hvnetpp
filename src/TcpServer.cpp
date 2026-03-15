#include "hvnetpp/TcpServer.h"
#include "hvnetpp/EventLoop.h"
#include "hvnetpp/Channel.h"
#include "hvnetpp/SocketsOps.h"
#include "hvnetpp/InetAddress.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <cerrno>
#include <cstdio>

namespace hvnetpp {

// Internal Acceptor class
class Acceptor {
public:
    using NewConnectionCallback = std::function<void(int sockfd, const InetAddress&)>;

    Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reuseport)
        : loop_(loop),
          acceptSocket_(sockets::createNonblockingOrDie(listenAddr.family())),
          acceptChannel_(loop, acceptSocket_),
          listening_(false),
          idleFd_(::open("/dev/null", O_RDONLY | O_CLOEXEC)) {
        
        assert(idleFd_ >= 0);
        sockets::setReuseAddr(acceptSocket_, true);
        sockets::setReusePort(acceptSocket_, reuseport);
        sockets::bindOrDie(acceptSocket_, listenAddr.getSockAddr());
        
        acceptChannel_.setReadCallback(std::bind(&Acceptor::handleRead, this));
    }

    ~Acceptor() {
        acceptChannel_.disableAll();
        acceptChannel_.remove();
        ::close(acceptSocket_);
        ::close(idleFd_);
    }

    void listen() {
        loop_->assertInLoopThread();
        listening_ = true;
        sockets::listenOrDie(acceptSocket_);
        acceptChannel_.enableReading();
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
                ::close(idleFd_);
                idleFd_ = ::accept(acceptSocket_, NULL, NULL);
                ::close(idleFd_);
                idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            }
            break;
        }
    }

    EventLoop* loop_;
    int acceptSocket_;
    Channel acceptChannel_;
    NewConnectionCallback newConnectionCallback_;
    bool listening_;
    int idleFd_;
};

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listenAddr, const std::string& nameArg)
    : loop_(loop),
      name_(nameArg),
      acceptor_(new Acceptor(loop, listenAddr, true)),
      nextConnId_(1) {
    acceptor_->setNewConnectionCallback(std::bind(&TcpServer::newConnection, this, std::placeholders::_1, std::placeholders::_2));
}

TcpServer::~TcpServer() {
    loop_->assertInLoopThread();
    for (auto& item : connections_) {
        TcpConnectionPtr conn(item.second);
        item.second.reset();
        conn->getLoop()->runInLoop(std::bind(&TcpConnection::connectDestroyed, conn));
    }
}

void TcpServer::start() {
    loop_->runInLoop(std::bind(&Acceptor::listen, acceptor_.get()));
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
    conn->setCloseCallback(std::bind(&TcpServer::removeConnection, this, std::placeholders::_1));
    
    loop_->runInLoop(std::bind(&TcpConnection::connectEstablished, conn));
}

void TcpServer::removeConnection(const TcpConnectionPtr& conn) {
    loop_->runInLoop(std::bind(&TcpServer::removeConnectionInLoop, this, conn));
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn) {
    loop_->assertInLoopThread();
    size_t n = connections_.erase(conn->name());
    assert(n == 1);
    loop_->queueInLoop(std::bind(&TcpConnection::connectDestroyed, conn));
}

} // namespace hvnetpp
