#pragma once

// 内部类：监听 socket 的封装，接受新连接后回调上层
#include "hvnetpp/InetAddress.h"

#include <functional>
#include <memory>

namespace hvnetpp {

class Channel;
class EventLoop;

class Acceptor : public std::enable_shared_from_this<Acceptor> {
public:
    using NewConnectionCallback = std::function<void(int sockfd, const InetAddress&)>;

    // 收养一个已 bind 好的非阻塞监听 fd；所有方法须在 loop 线程调用（构造除外）
    Acceptor(EventLoop* loop, int listenfd);
    ~Acceptor();

    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    void setNewConnectionCallback(const NewConnectionCallback& cb) { newConnectionCallback_ = cb; }
    void tieChannel();
    void listen();
    bool listening() const { return listening_; }

private:
    void handleRead();

    EventLoop* loop_;
    int acceptSocket_;
    std::shared_ptr<Channel> acceptChannel_;
    NewConnectionCallback newConnectionCallback_;
    bool listening_;
    int idleFd_; // EMFILE 兜底
};

} // namespace hvnetpp
