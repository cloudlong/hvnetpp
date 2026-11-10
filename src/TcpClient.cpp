#include "hvnetpp/TcpClient.h"
#include "Connector.h"
#include "hvnetpp/EventLoop.h"
#include "hvnetpp/EventLoopThread.h"
#include "hvnetpp/SocketsOps.h"
#include "rtclog.h"

#include <arpa/inet.h>
#include <assert.h>
#include <cstring>

namespace hvnetpp {

uint32_t reconn_setting_calc_delay(reconn_setting_t* reconn) {
    switch (reconn->delay_policy) {
    case 0: // 固定
        reconn->cur_delay = reconn->min_delay;
        break;
    case 1: // 线性
        reconn->cur_delay += reconn->min_delay;
        break;
    default: // 指数
        reconn->cur_delay = (reconn->cur_delay == 0) ? reconn->min_delay : reconn->cur_delay * 2;
        break;
    }
    if (reconn->cur_delay > reconn->max_delay) {
        reconn->cur_delay = reconn->max_delay;
    }
    if (reconn->cur_delay == 0) {
        reconn->cur_delay = 1000;
    }
    return reconn->cur_delay;
}

void reconn_setting_reset(reconn_setting_t* reconn) {
    reconn->cur_retry_cnt = 0;
    reconn->cur_delay = 0;
}

TcpClient::TcpClient(std::string name)
    : name_(std::move(name)),
      loopThread_(new EventLoopThread(EventLoopThread::ThreadInitCallback(), name_ + "-loop")) {
}

TcpClient::TcpClient(EventLoop* loop, std::string name)
    : name_(std::move(name)),
      loop_(loop) {
}

TcpClient::~TcpClient() {
    if (started_) {
        stop();
    }
}

int TcpClient::createsocket(int port, const char* host) {
    assert(!started_);
    if (host == nullptr) {
        host = "127.0.0.1";
    }
    struct in_addr addr4;
    struct in6_addr addr6;
    if (::inet_pton(AF_INET, host, &addr4) == 1) {
        remoteAddr_ = InetAddress(host, static_cast<uint16_t>(port), false);
    } else if (::inet_pton(AF_INET6, host, &addr6) == 1) {
        remoteAddr_ = InetAddress(host, static_cast<uint16_t>(port), true);
    } else {
        // 域名解析
        InetAddress resolved(static_cast<uint16_t>(port));
        if (!InetAddress::resolve(host, &resolved)) {
            RTCLOG(RTC_ERROR, "TcpClient[%s] resolve %s failed", name_.c_str(), host);
            return -1;
        }
        remoteAddr_ = resolved;
    }
    hasRemoteAddr_ = true;
    return 0;
}

void TcpClient::setReconnect(const reconn_setting_t* setting) {
    if (setting) {
        reconn_.reset(new reconn_setting_t(*setting));
        reconn_setting_reset(reconn_.get());
    } else {
        reconn_.reset();
    }
}

void TcpClient::setUnpack(const unpack_setting_t* setting) {
    if (setting && setting->mode != UNPACK_MODE_NONE) {
        unpack_.reset(new unpack_setting_t(*setting));
    } else {
        unpack_.reset();
    }
}

void TcpClient::setHeartbeat(int intervalMs, std::function<void()> fn) {
    heartbeatIntervalMs_ = intervalMs;
    heartbeatFn_ = std::move(fn);
}

int TcpClient::start(bool wait_threads_started) {
    if (!hasRemoteAddr_) {
        RTCLOG(RTC_ERROR, "TcpClient[%s] start without createsocket", name_.c_str());
        return -1;
    }
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return 0;
    }

    if (loopThread_) {
        loop_ = loopThread_->startLoop();
    }
    auto setup = [this] {
        if (!started_.load(std::memory_order_acquire)) {
            return;   // start(false) 之后、本任务执行之前已在 loop 线程内 stop()
        }
        connector_ = std::make_shared<Connector>(loop_, remoteAddr_);
        connector_->setConnectTimeout(connectTimeoutMs_);
        connector_->setNewConnectionCallback(
            std::bind(&TcpClient::onConnectorSuccess, this, std::placeholders::_1));
        connector_->setConnectFailedCallback(
            std::bind(&TcpClient::onConnectorFail, this));
        connector_->startConnect();
    };
    if (wait_threads_started) {
        loop_->runInLoopAndWait(setup);
    } else {
        loop_->runInLoop(setup);   // 旧实现在这里把局部 promise 的引用交给 loop 线程，返回后悬空
    }
    return 0;
}

void TcpClient::stop(bool wait_threads_stopped) {
    (void)wait_threads_stopped;
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false)) {
        return;
    }
    assert(!(loopThread_ && loop_->isInLoopThread()) && "TcpClient::stop() 不可在本客户端的回调里调用");

    const bool ran = loop_->runInLoopAndWait([this] {
        if (hasReconnectTimer_) {
            loop_->cancel(reconnectTimer_);
            hasReconnectTimer_ = false;
        }
        if (connector_) {
            connector_->stopConnect();
        }
        SocketChannelPtr channel;
        {
            std::lock_guard<std::mutex> lock(channelMutex_);
            channel.swap(channel_);
        }
        if (channel) {
            channel->forceCloseInLoop();
        }
        // 外部 loop 模式下 stop() 可能就在 Connector 的回调链里（onConnection / onConnectFailed），延迟到下一轮再析构
        std::shared_ptr<Connector> connector = std::move(connector_);
        if (connector) {
            loop_->queueInLoop([connector] {});
        }
    });
    if (!ran) {
        // loop 已退出：连接对象绑定在已退出的 loop 上，不能在本线程注销，只能放弃
        RTCLOG(RTC_ERROR, "TcpClient[%s] 所在 loop 已退出，请先 stop() 客户端再停止外部 loop", name_.c_str());
        static_cast<void>(new std::shared_ptr<Connector>(std::move(connector_)));   // 有意泄漏
        std::lock_guard<std::mutex> lock(channelMutex_);
        static_cast<void>(new SocketChannelPtr(std::move(channel_)));               // 有意泄漏
    }

    if (loopThread_) {
        loopThread_->stop();
        loop_ = nullptr;
    }
    RTCLOG(RTC_INFO, "TcpClient[%s] stopped", name_.c_str());
}

void TcpClient::onConnectorSuccess(int sockfd) {
    loop_->assertInLoopThread();
    if (!started_) {
        sockets::close(sockfd);
        return;
    }

    InetAddress localAddr(sockets::getLocalAddr(sockfd));
    SocketChannelPtr channel = std::make_shared<SocketChannel>(loop_, sockfd, localAddr, remoteAddr_);
    if (unpack_) {
        channel->setUnpack(unpack_.get());
    }
    if (keepaliveTimeoutMs_ > 0) {
        channel->setKeepaliveTimeout(keepaliveTimeoutMs_);
    }
    if (heartbeatIntervalMs_ > 0 && heartbeatFn_) {
        channel->setHeartbeat(heartbeatIntervalMs_, heartbeatFn_);
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
    channel->setCloseCallback(std::bind(&TcpClient::onChannelClose, this, std::placeholders::_1));

    {
        std::lock_guard<std::mutex> lock(channelMutex_);
        channel_ = channel;
    }
    if (reconn_) {
        reconn_setting_reset(reconn_.get());
    }
    channel->connectEstablished();
}

void TcpClient::onConnectorFail() {
    loop_->assertInLoopThread();
    if (onConnectFailed) {
        onConnectFailed();
    }
    scheduleReconnect();
}

void TcpClient::onChannelClose(const SocketChannelPtr& channel) {
    loop_->assertInLoopThread();
    {
        std::lock_guard<std::mutex> lock(channelMutex_);
        channel_.reset();
    }
    channel->loop()->queueInLoop([channel] { channel->connectDestroyed(); });
    scheduleReconnect();
}

void TcpClient::scheduleReconnect() {
    loop_->assertInLoopThread();
    if (!started_ || !reconn_) {
        return;
    }
    if (reconn_->max_retry_cnt > 0 && reconn_->cur_retry_cnt >= reconn_->max_retry_cnt) {
        RTCLOG(RTC_ERROR, "TcpClient[%s] give up reconnecting to %s after %u retries",
               name_.c_str(), remoteAddr_.toIpPort().c_str(), reconn_->cur_retry_cnt);
        return;
    }
    ++reconn_->cur_retry_cnt;
    uint32_t delayMs = reconn_setting_calc_delay(reconn_.get());
    RTCLOG(RTC_INFO, "TcpClient[%s] reconnect to %s in %u ms (retry %u)",
           name_.c_str(), remoteAddr_.toIpPort().c_str(), delayMs, reconn_->cur_retry_cnt);
    reconnectTimer_ = loop_->runAfter(delayMs / 1000.0, [this] {
        hasReconnectTimer_ = false;
        if (started_ && connector_) {
            connector_->startConnect();
        }
    });
    hasReconnectTimer_ = true;
}

bool TcpClient::isConnected() const {
    SocketChannelPtr ch = channel();
    return ch && ch->isConnected();
}

int TcpClient::send(const void* data, size_t len) {
    SocketChannelPtr ch = channel();
    if (ch && ch->isConnected()) {
        return ch->write(data, len);
    }
    return -1;
}

int TcpClient::send(const std::string& msg) {
    return send(msg.data(), msg.size());
}

SocketChannelPtr TcpClient::channel() const {
    std::lock_guard<std::mutex> lock(channelMutex_);
    return channel_;
}

} // namespace hvnetpp
