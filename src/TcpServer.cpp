#include "hvnetpp/TcpServer.h"
#include "Acceptor.h"
#include "hvnetpp/EventLoop.h"
#include "hvnetpp/EventLoopThread.h"
#include "hvnetpp/EventLoopThreadPool.h"
#include "hvnetpp/SocketsOps.h"
#include "rtclog.h"

#include <assert.h>
#include <cerrno>
#include <cstring>
#include <string.h>
#include <unistd.h>
#include <vector>

namespace hvnetpp {

TcpServer::TcpServer(std::string name)
    : name_(std::move(name)),
      acceptorThread_(new EventLoopThread(EventLoopThread::ThreadInitCallback(), name_ + "-accept")) {
}

TcpServer::TcpServer(EventLoop* loop, std::string name)
    : name_(std::move(name)),
      acceptorLoop_(loop) {
}

TcpServer::~TcpServer() {
    if (started_) {
        stop();
    }
    closesocket();
}

int TcpServer::createsocket(int port, const char* host) {
    assert(!started_);
    if (listenfd_ >= 0) {
        closesocket();
    }
    bool ipv6 = (host != nullptr && ::strchr(host, ':') != nullptr);
    listenAddr_ = InetAddress(host ? host : "0.0.0.0", static_cast<uint16_t>(port), ipv6);
    int fd = sockets::createNonblockingOrDie(listenAddr_.family());
    sockets::setReuseAddr(fd, true);
    if (reusePort_) {
        // 默认不开：否则误起第二个进程也能 bind 成功，连接被内核分给两个进程，极难排查
        sockets::setReusePort(fd, true);
    }
    socklen_t addrlen = ipv6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    if (::bind(fd, listenAddr_.getSockAddr(), addrlen) < 0) {
        RTCLOG(RTC_ERROR, "TcpServer[%s] bind %s failed: %s",
               name_.c_str(), listenAddr_.toIpPort().c_str(), strerror(errno));
        sockets::close(fd);
        return -1;
    }
    listenfd_ = fd;
    return listenfd_;
}

void TcpServer::closesocket() {
    if (listenfd_ >= 0 && !acceptor_) {
        // fd 已交给 Acceptor 时由 Acceptor 析构关闭
        sockets::close(listenfd_);
    }
    listenfd_ = -1;
}

void TcpServer::setUnpack(const unpack_setting_t* setting) {
    if (setting && setting->mode != UNPACK_MODE_NONE) {
        unpack_.reset(new unpack_setting_t(*setting));
    } else {
        unpack_.reset();
    }
}

int TcpServer::start(bool wait_threads_started) {
    (void)wait_threads_started; // 线程启动本身就是同步等待的，参数为对齐 libhv API 保留
    if (listenfd_ < 0) {
        RTCLOG(RTC_ERROR, "TcpServer[%s] start without createsocket", name_.c_str());
        return -1;
    }
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return 0;
    }

    if (acceptorThread_) {
        acceptorLoop_ = acceptorThread_->startLoop();
    }
    acceptorLoop_->runInLoopAndWait([this] {
        if (externalPool_) {
            ioLoops_ = externalPool_->getAllLoops();
        } else {
            workerPool_.reset(new EventLoopThreadPool(acceptorLoop_, name_));
            workerPool_->setThreadNum(threadNum_);
            workerPool_->start();
            ioLoops_ = workerPool_->getAllLoops();
        }
        nextLoop_ = 0;
        acceptor_ = std::make_shared<Acceptor>(acceptorLoop_, listenfd_);
        acceptor_->tieChannel();
        acceptor_->setNewConnectionCallback(
            std::bind(&TcpServer::newConnection, this, std::placeholders::_1, std::placeholders::_2));
        acceptor_->listen();
    });
    RTCLOG(RTC_INFO, "TcpServer[%s] listening on %s with %zu io loop(s)",
           name_.c_str(), listenAddr_.toIpPort().c_str(), ioLoops_.size());
    return 0;
}

bool TcpServer::inOwnThread() const {
    if (acceptorThread_ && acceptorLoop_ && acceptorLoop_->isInLoopThread()) {
        return true;
    }
    if (workerPool_) {
        EventLoop* current = EventLoop::getEventLoopOfCurrentThread();
        for (EventLoop* l : ioLoops_) {
            if (l == current && l != acceptorLoop_) {
                return true;
            }
        }
    }
    return false;
}

void TcpServer::stop(bool wait_threads_stopped) {
    (void)wait_threads_stopped;
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false)) {
        return;
    }
    // 自建线程里调用会在同步等待 / join 自己时死锁
    assert(!inOwnThread() && "TcpServer::stop() 不可在本服务器的回调里调用");

    // 1. 停止接受新连接
    if (!acceptorLoop_->runInLoopAndWait([this] { acceptor_.reset(); })) {
        // accept loop 已退出：Acceptor 的 Channel 绑定在该 loop 上，不能在本线程注销，只能放弃
        RTCLOG(RTC_ERROR, "TcpServer[%s] accept loop 已退出，请先 stop() 服务器再停止外部 loop", name_.c_str());
        static_cast<void>(new std::shared_ptr<Acceptor>(std::move(acceptor_)));   // 有意泄漏
    }
    listenfd_ = -1; // fd 已随 Acceptor 关闭

    // 2. 强制关闭全部连接（在各自 loop 内一步完成 close + destroy）
    std::vector<SocketChannelPtr> channels;
    {
        std::lock_guard<std::mutex> lock(connMutex_);
        for (auto& item : connections_) {
            channels.push_back(item.second);
        }
        connections_.clear();
    }
    for (auto& ch : channels) {
        ch->loop()->runInLoop([ch] { ch->forceCloseInLoop(); });
    }
    channels.clear();

    // 3. 栅栏：同一线程投递的任务按序执行，等到它就说明上面的关闭任务都已完成
    for (EventLoop* loop : ioLoops_) {
        loop->runInLoopAndWait([] {});
    }
    ioLoops_.clear();

    // 4/5. 停自建的 worker 线程与 accept 线程（外部提供的不动）
    if (workerPool_) {
        workerPool_->stop();
        workerPool_.reset();
    }
    if (acceptorThread_) {
        acceptorThread_->stop();
        acceptorLoop_ = nullptr;
    }
    RTCLOG(RTC_INFO, "TcpServer[%s] stopped", name_.c_str());
}

void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr) {
    acceptorLoop_->assertInLoopThread();

    uint32_t maxConn = maxConnections_.load();
    if (maxConn > 0 && connectionNum() >= maxConn) {
        RTCLOG(RTC_WARN, "TcpServer[%s] reach max connection num %u, reject %s",
               name_.c_str(), maxConn, peerAddr.toIpPort().c_str());
        sockets::close(sockfd);
        return;
    }

    EventLoop* ioLoop = ioLoops_[nextLoop_];
    nextLoop_ = (nextLoop_ + 1) % ioLoops_.size();
    InetAddress localAddr(sockets::getLocalAddr(sockfd));
    SocketChannelPtr channel = std::make_shared<SocketChannel>(ioLoop, sockfd, localAddr, peerAddr);
    if (unpack_) {
        channel->setUnpack(unpack_.get());
    }
    if (keepaliveTimeoutMs_ > 0) {
        channel->setKeepaliveTimeout(keepaliveTimeoutMs_);
    }
    channel->setConnectionCallback([this](const SocketChannelPtr& ch) {
        if (onConnection) onConnection(ch);
    });
    channel->setMessageCallback([this](const SocketChannelPtr& ch, Buffer* buf) {
        if (onMessage) onMessage(ch, buf);
    });
    channel->setWriteCompleteCallback([this](const SocketChannelPtr& ch) {
        if (onWriteComplete) onWriteComplete(ch);
    });
    channel->setCloseCallback(std::bind(&TcpServer::removeChannel, this, std::placeholders::_1));

    {
        std::lock_guard<std::mutex> lock(connMutex_);
        connections_[channel->id()] = channel;
    }
    ioLoop->runInLoop([channel] { channel->connectEstablished(); });
}

void TcpServer::removeChannel(const SocketChannelPtr& channel) {
    {
        std::lock_guard<std::mutex> lock(connMutex_);
        connections_.erase(channel->id());
    }
    channel->loop()->queueInLoop([channel] { channel->connectDestroyed(); });
}

size_t TcpServer::connectionNum() const {
    std::lock_guard<std::mutex> lock(connMutex_);
    return connections_.size();
}

void TcpServer::foreachChannel(const std::function<void(const SocketChannelPtr&)>& fn) {
    std::vector<SocketChannelPtr> channels;
    {
        std::lock_guard<std::mutex> lock(connMutex_);
        for (auto& item : connections_) {
            channels.push_back(item.second);
        }
    }
    for (auto& ch : channels) {
        fn(ch);
    }
}

void TcpServer::broadcast(const void* data, size_t len) {
    foreachChannel([data, len](const SocketChannelPtr& ch) {
        ch->write(data, len);
    });
}

void TcpServer::broadcast(const std::string& msg) {
    broadcast(msg.data(), msg.size());
}

EventLoop* TcpServer::loop() {
    return acceptorLoop_;
}

} // namespace hvnetpp
