#pragma once

#include "hvnetpp/Buffer.h"
#include "hvnetpp/InetAddress.h"
#include <atomic>
#include <memory>
#include <string>
#include <functional>
#include <netinet/in.h>

namespace hvnetpp {

class EventLoop;
class Channel;

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
    using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
    // Runs on the owning loop thread. The buffer is connection-owned and callbacks should retrieve bytes they consume.
    using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*)>;
    using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;
    using HighWaterMarkCallback = std::function<void(const TcpConnectionPtr&, size_t)>;

    TcpConnection(EventLoop* loop,
                  const std::string& name,
                  int sockfd,
                  const InetAddress& localAddr,
                  const InetAddress& peerAddr);
    ~TcpConnection();

    EventLoop* getLoop() const { return loop_; }
    const std::string& name() const { return name_; }
    const InetAddress& localAddress() const { return localAddr_; }
    const InetAddress& peerAddress() const { return peerAddr_; }
    bool connected() const { return state() == kConnected; }

    // Safe to call from any thread. Copies bytes into the connection send path.
    void send(const std::string& message);
    // Safe to call from any thread. Copies bytes into the connection send path.
    void send(const void* data, size_t len);
    // Safe to call from any thread. Does not mutate `message`.
    void send(const Buffer& message);
    // Compatibility overload. Transfers and clears the readable bytes from `message`.
    void send(Buffer* message);
    // Safe to call from any thread. Flushes pending output before half-closing.
    void shutdown();
    void setTcpNoDelay(bool on);

    void setConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) { writeCompleteCallback_ = cb; }
    void setHighWaterMarkCallback(const HighWaterMarkCallback& cb, size_t highWaterMark) { highWaterMarkCallback_ = cb; highWaterMark_ = highWaterMark; }

private:
    friend class TcpServer;

    using CloseCallback = std::function<void(const TcpConnectionPtr&)>;
    enum StateE { kDisconnected, kConnecting, kConnected, kDisconnecting };

    void setCloseCallback(const CloseCallback& cb) { closeCallback_ = cb; }
    void connectEstablished();
    void connectDestroyed();
    void handleRead();
    void handleWrite();
    void handleClose();
    void handleError();
    void handleError(int err);
    void sendInLoop(const std::string& message);
    void sendInLoop(const void* message, size_t len);
    void shutdownInLoop();
    void closeSocket();
    StateE state() const { return state_.load(std::memory_order_acquire); }
    void setState(StateE s) { state_.store(s, std::memory_order_release); }

    EventLoop* loop_;
    const std::string name_;
    std::atomic<StateE> state_;
    
    // We hold the fd but Channel doesn't own it.
    std::unique_ptr<Channel> channel_;
    int socketFd_;
    
    InetAddress localAddr_;
    InetAddress peerAddr_;

    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;
    CloseCallback closeCallback_;
    size_t highWaterMark_;

    Buffer inputBuffer_;
    Buffer outputBuffer_;
};

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

} // namespace hvnetpp
