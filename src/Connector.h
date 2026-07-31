#pragma once

// 内部类：非阻塞 connect 状态机，单次连接尝试；重试策略由 TcpClient 决定。
// 所有方法须在所属 loop 线程调用。
#include "hvnetpp/InetAddress.h"
#include "hvnetpp/TimerId.h"

#include <functional>
#include <memory>

namespace hvnetpp {

class Channel;
class EventLoop;

class Connector : public std::enable_shared_from_this<Connector> {
public:
    using NewConnectionCallback = std::function<void(int sockfd)>;
    using ConnectFailedCallback = std::function<void()>;

    Connector(EventLoop* loop, const InetAddress& serverAddr);
    ~Connector();

    Connector(const Connector&) = delete;
    Connector& operator=(const Connector&) = delete;

    void setConnectTimeout(int timeoutMs) { connectTimeoutMs_ = timeoutMs; }
    void setNewConnectionCallback(const NewConnectionCallback& cb) { newConnectionCallback_ = cb; }
    void setConnectFailedCallback(const ConnectFailedCallback& cb) { connectFailedCallback_ = cb; }

    void startConnect();   // 发起一次连接尝试（每次新建 fd）
    void stopConnect();    // 取消进行中的尝试

private:
    enum State { kIdle, kConnecting };

    void connecting(int sockfd);
    void handleWrite();
    void handleError();
    void onConnectTimeout();
    int removeAndResetChannel();   // 返回 fd；Channel 延迟析构
    void cancelTimeoutTimer();
    void reportFailure();

    EventLoop* loop_;
    InetAddress serverAddr_;
    State state_ = kIdle;
    std::shared_ptr<Channel> channel_;
    int connectTimeoutMs_ = 5000;
    TimerId timeoutTimer_;
    bool hasTimeoutTimer_ = false;
    NewConnectionCallback newConnectionCallback_;
    ConnectFailedCallback connectFailedCallback_;
};

} // namespace hvnetpp
