#pragma once

#include "hvnetpp/Buffer.h"
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
    // Runs on the owning loop thread. The buffer is socket-owned and contains one datagram per callback.
    using ReadCallback = std::function<void(const InetAddress& peerAddr, Buffer* buf)>;
    
    UdpSocket(EventLoop* loop, const std::string& name);
    ~UdpSocket();

    // Safe to call from any thread. Read-interest registration runs on the loop thread.
    // Returns false if socket creation/bind fails.
    bool bind(const InetAddress& addr);
    void setReadCallback(ReadCallback cb) { readCallback_ = std::move(cb); }
    
    // Send data to destination
    ssize_t sendTo(const void* data, size_t len, const InetAddress& destAddr);
    // Transfers the readable bytes from `buf` into the socket send path.
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
    Buffer inputBuffer_;
};

} // namespace hvnetpp
