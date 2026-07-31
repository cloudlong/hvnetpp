#pragma once

#include <memory>
#include <functional>
#include <vector>
#include "hvnetpp/InetAddress.h"

namespace hvnetpp {

class EventLoop;
class Channel;
class Buffer;

class UdpSocket {
public:
    using ReadCallback = std::function<void(const InetAddress& peerAddr, Buffer* buf)>;
    
    UdpSocket(EventLoop* loop, const std::string& name);
    // 收养一个已创建（可能已 bind/connect）的非阻塞 UDP fd，fd 生命周期交由本对象管理
    UdpSocket(EventLoop* loop, const std::string& name, int sockfd, sa_family_t family);
    ~UdpSocket();

    bool bind(const InetAddress& addr);
    // 开始监听可读事件（收养 fd 场景使用）；须在 loop 线程调用
    void startReading();
    void setReadCallback(ReadCallback cb) { readCallback_ = std::move(cb); }
    
    // Send data to destination
    ssize_t sendTo(const void* data, size_t len, const InetAddress& destAddr);
    ssize_t sendTo(Buffer* buf, const InetAddress& destAddr);

    int fd() const { return sockfd_; }

private:
    bool ensureSocket(sa_family_t family);
    void handleRead();

    EventLoop* loop_;
    const std::string name_;
    sa_family_t family_;
    int sockfd_;
    std::shared_ptr<Channel> channel_;
    std::shared_ptr<bool> callbackToken_;
    ReadCallback readCallback_;
    std::vector<char> readBuf_; // UDP packet buffer
};

} // namespace hvnetpp
