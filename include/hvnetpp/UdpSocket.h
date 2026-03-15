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
    ~UdpSocket();

    bool bind(const InetAddress& addr);
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
    std::unique_ptr<Channel> channel_;
    ReadCallback readCallback_;
    std::vector<char> readBuf_; // UDP packet buffer
};

} // namespace hvnetpp
