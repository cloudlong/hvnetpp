#pragma once

#include "hvnetpp/Callbacks.h"
#include "hvnetpp/EventLoopThread.h"
#include "hvnetpp/InetAddress.h"
#include "hvnetpp/SocketChannel.h"
#include "hvnetpp/Unpack.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace hvnetpp {

class Acceptor;
class EventLoop;
class EventLoopThreadPool;

// libhv evpp 风格 TCP 服务端：
//   TcpServer srv("echo");
//   srv.createsocket(1234);
//   srv.setThreadNum(4);
//   srv.onConnection = [](const SocketChannelPtr& ch) { ... }; // 建立/断开都回调，用 isConnected() 区分
//   srv.onMessage    = [](const SocketChannelPtr& ch, Buffer* buf) { ... };
//   srv.start();
// 内部自带 accept 线程 + worker 线程池；start() 返回后即开始服务。
class TcpServer {
public:
    explicit TcpServer(std::string name = "TcpServer");
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // 创建监听 socket 并 bind（listen 推迟到 start()）。成功返回 listenfd，失败返回 -1。
    int createsocket(int port, const char* host = "0.0.0.0");
    void closesocket();

    // 以下配置须在 start() 之前调用
    void setThreadNum(int num) { threadNum_ = num; }                  // 0 = 所有 IO 在 accept 线程
    void setMaxConnectionNum(uint32_t num) { maxConnections_ = num; } // 0 = 不限
    void setUnpack(const unpack_setting_t* setting);
    void setKeepaliveTimeout(int timeoutMs) { keepaliveTimeoutMs_ = timeoutMs; }

    void start(bool wait_threads_started = true);
    // 停止接受新连接、关闭全部连接、join 所有线程。不可在本服务器的回调里调用。
    void stop(bool wait_threads_stopped = true);

    // 用户回调（start() 前设置）
    ConnectionCallback onConnection;
    MessageCallback onMessage;
    WriteCompleteCallback onWriteComplete;

    // 以下接口线程安全
    size_t connectionNum() const;
    void foreachChannel(const std::function<void(const SocketChannelPtr&)>& fn);
    void broadcast(const void* data, size_t len);
    void broadcast(const std::string& msg);

    EventLoop* loop();          // accept 线程的 loop；未 start 时为 nullptr
    const std::string& name() const { return name_; }
    int listenfd() const { return listenfd_; }

private:
    void newConnection(int sockfd, const InetAddress& peerAddr);  // accept 线程
    void removeChannel(const SocketChannelPtr& channel);

    std::string name_;
    EventLoopThread acceptorThread_;
    EventLoop* acceptorLoop_ = nullptr;
    std::unique_ptr<EventLoopThreadPool> workerPool_;
    std::shared_ptr<Acceptor> acceptor_;

    int listenfd_ = -1;
    InetAddress listenAddr_;
    int threadNum_ = 0;
    std::atomic<uint32_t> maxConnections_{0};
    std::unique_ptr<unpack_setting_t> unpack_;
    int keepaliveTimeoutMs_ = 0;

    mutable std::mutex connMutex_;
    std::map<uint32_t, SocketChannelPtr> connections_;
    std::atomic<bool> started_{false};
};

} // namespace hvnetpp
