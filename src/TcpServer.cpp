#include "hvnetpp/TcpServer.h"
#include "Acceptor.h"
#include "hvnetpp/EventLoop.h"
#include "hvnetpp/EventLoopThreadPool.h"
#include "hvnetpp/SocketsOps.h"
#include "rtclog.h"

#include <assert.h>
#include <cerrno>
#include <cstring>
#include <future>
#include <string.h>
#include <unistd.h>
#include <vector>

namespace hvnetpp {

TcpServer::TcpServer(std::string name)
    : name_(std::move(name)),
      acceptorThread_(EventLoopThread::ThreadInitCallback(), name_ + "-acceptor") {
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
    sockets::setReusePort(fd, true);
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

void TcpServer::start(bool wait_threads_started) {
    (void)wait_threads_started; // 线程启动本身就是同步等待的，参数为对齐 libhv API 保留
    if (listenfd_ < 0) {
        RTCLOG(RTC_ERROR, "TcpServer[%s] start without createsocket", name_.c_str());
        return;
    }
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return;
    }

    acceptorLoop_ = acceptorThread_.startLoop();
    workerPool_.reset(new EventLoopThreadPool(acceptorLoop_, name_));
    workerPool_->setThreadNum(threadNum_);

    std::promise<void> started;
    acceptorLoop_->runInLoop([this, &started] {
        workerPool_->start();
        acceptor_ = std::make_shared<Acceptor>(acceptorLoop_, listenfd_);
        acceptor_->tieChannel();
        acceptor_->setNewConnectionCallback(
            std::bind(&TcpServer::newConnection, this, std::placeholders::_1, std::placeholders::_2));
        acceptor_->listen();
        started.set_value();
    });
    started.get_future().wait();
    RTCLOG(RTC_INFO, "TcpServer[%s] listening on %s with %d worker thread(s)",
           name_.c_str(), listenAddr_.toIpPort().c_str(), threadNum_);
}

void TcpServer::stop(bool wait_threads_stopped) {
    (void)wait_threads_stopped;
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false)) {
        return;
    }
    // 禁止在本服务器的回调（即自有 loop 线程）里调用 stop，否则下面的同步等待会死锁
    assert(!acceptorLoop_->isInLoopThread());

    // 1. 停止接受新连接
    {
        std::promise<void> done;
        acceptorLoop_->runInLoop([this, &done] {
            acceptor_.reset();
            done.set_value();
        });
        done.get_future().wait();
        listenfd_ = -1; // fd 已随 Acceptor 关闭
    }

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

    // 3. 栅栏：确保上面的关闭任务都已执行完
    std::vector<std::future<void>> barriers;
    for (EventLoop* loop : workerPool_->getAllLoops()) {
        auto p = std::make_shared<std::promise<void>>();
        barriers.push_back(p->get_future());
        loop->queueInLoop([p] { p->set_value(); });
    }
    for (auto& f : barriers) {
        f.wait();
    }

    // 4/5. 停 worker 线程与 accept 线程
    workerPool_->stop();
    acceptorThread_.stop();
    acceptorLoop_ = nullptr;
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

    EventLoop* ioLoop = workerPool_->getNextLoop();
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
