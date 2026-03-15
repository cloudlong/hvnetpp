#pragma once

#include "hvnetpp/TcpConnection.h"
#include <atomic>
#include <map>
#include <memory>
#include <string>

namespace hvnetpp {
class EventLoop;
class Acceptor; // Helper for accept()
class InetAddress;

class TcpServer {
public:
    enum class State {
        kIdle,
        kStarting,
        kListening,
    };

    using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
    using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*)>;
    using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;

    TcpServer(EventLoop* loop, const InetAddress& listenAddr, const std::string& nameArg);
    ~TcpServer();

    // Safe to call from any thread.
    // Returns kListening when listen() completed now, kStarting when queued to the loop thread,
    // and kIdle if an immediate start attempt failed.
    State start();
    State state() const { return state_.load(std::memory_order_acquire); }
    bool listening() const { return state() == State::kListening; }

    void setConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) { writeCompleteCallback_ = cb; }

private:
    void newConnection(int sockfd, const InetAddress& peerAddr);
    void removeConnection(const TcpConnectionPtr& conn);
    void removeConnectionInLoop(const TcpConnectionPtr& conn);

    using ConnectionMap = std::map<std::string, TcpConnectionPtr>;

    EventLoop* loop_;
    const std::string name_;
    
    std::shared_ptr<Acceptor> acceptor_; // Internal class to handle bind/listen/accept
    
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    std::shared_ptr<bool> callbackToken_;
    std::atomic<State> state_;

    int nextConnId_;
    ConnectionMap connections_;
};

} // namespace hvnetpp
