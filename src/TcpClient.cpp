#include "hvnetpp/TcpClient.h"
#include "Connector.h"
#include "hvnetpp/EventLoop.h"
#include "hvnetpp/SocketsOps.h"
#include "rtclog.h"

#include <arpa/inet.h>
#include <assert.h>
#include <cstring>
#include <future>

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
      loopThread_(EventLoopThread::ThreadInitCallback(), name_ + "-loop") {
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

void TcpClient::start(bool wait_threads_started) {
    if (!hasRemoteAddr_) {
        RTCLOG(RTC_ERROR, "TcpClient[%s] start without createsocket", name_.c_str());
        return;
    }
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return;
    }

    loop_ = loopThread_.startLoop();

    std::promise<void> ready;
    loop_->runInLoop([this, &ready] {
        connector_ = std::make_shared<Connector>(loop_, remoteAddr_);
        connector_->setConnectTimeout(connectTimeoutMs_);
        connector_->setNewConnectionCallback(
            std::bind(&TcpClient::onConnectorSuccess, this, std::placeholders::_1));
        connector_->setConnectFailedCallback(
            std::bind(&TcpClient::onConnectorFail, this));
        connector_->startConnect();
        ready.set_value();
    });
    if (wait_threads_started) {
        ready.get_future().wait();
    }
}

void TcpClient::stop(bool wait_threads_stopped) {
    (void)wait_threads_stopped;
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false)) {
        return;
    }
    assert(loop_ != nullptr && !loop_->isInLoopThread());

    std::promise<void> done;
    loop_->runInLoop([this, &done] {
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
        connector_.reset();
        done.set_value();
    });
    done.get_future().wait();

    loopThread_.stop();
    loop_ = nullptr;
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
