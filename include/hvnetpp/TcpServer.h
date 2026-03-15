#pragma once

#include "hvnetpp/TcpConnection.h"
#include <map>
#include <memory>
#include <string>

namespace hvnetpp {
class EventLoop;
class Acceptor; // Helper for accept()
class InetAddress;

class TcpServer {
public:
    using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
    using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*)>;
    using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;

    TcpServer(EventLoop* loop, const InetAddress& listenAddr, const std::string& nameArg);
    ~TcpServer();

    // Safe to call from any thread. The actual listen step runs on the loop thread.
    // Socket setup failures are logged and leave the server idle instead of aborting the process.
    void start();
    
    void setConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) { writeCompleteCallback_ = cb; }

private:
    void newConnection(int sockfd, const InetAddress& peerAddr);
    void removeConnection(const TcpConnectionPtr& conn);
    void removeConnectionInLoop(const TcpConnectionPtr& conn);

    using ConnectionMap = std::map<std::string, TcpConnectionPtr>;

    EventLoop* loop_;
    const std::string ipPort_;
    const std::string name_;
    
    std::shared_ptr<Acceptor> acceptor_; // Internal class to handle bind/listen/accept
    
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    std::shared_ptr<bool> callbackToken_;
    
    int nextConnId_;
    ConnectionMap connections_;
};

} // namespace hvnetpp
