#pragma once

#include "hvnetpp/Callbacks.h"
#include "hvnetpp/InetAddress.h"
#include "hvnetpp/SocketChannel.h"
#include "hvnetpp/Unpack.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hvnetpp {

class Acceptor;
class EventLoop;
class EventLoopThread;
class EventLoopThreadPool;

// libhv evpp 风格 TCP 服务端：
//   TcpServer srv("echo");
//   srv.createsocket(1234);
//   srv.setThreadNum(4);
//   srv.onConnection = [](const SocketChannelPtr& ch) { ... }; // 建立/断开都回调，用 isConnected() 区分
//   srv.onMessage    = [](const SocketChannelPtr& ch, Buffer* buf) { ... };
//   srv.start();
// 默认自带 accept 线程 + worker 线程池；start() 返回后即开始服务。
// 也可以复用外部线程：TcpServer(loop) 在外部 loop 上 accept；setEventLoopThreadPool(pool) 让连接分配到外部线程池，
// 这样信令连接和同一会话的 UDP 媒体 / 定时器可以落在同一个 loop 线程里。
class TcpServer {
public:
    explicit TcpServer(std::string name = "TcpServer");
    // accept 跑在外部 loop 上（不创建 accept 线程，不拥有该 loop）
    explicit TcpServer(EventLoop* loop, std::string name = "TcpServer");
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // 创建监听 socket 并 bind（listen 推迟到 start()）。成功返回 listenfd，失败返回 -1。
    // 默认只开 SO_REUSEADDR；需要多进程共享端口时在此之前 setReusePort(true)。
    int createsocket(int port, const char* host = "0.0.0.0");
    void closesocket();

    // 以下配置须在 start() 之前调用
    void setThreadNum(int num) { threadNum_ = num; }                  // 0 = 所有 IO 在 accept 线程
    void setEventLoopThreadPool(EventLoopThreadPool* pool) { externalPool_ = pool; } // 外部 worker 池（须已 start），优先于 setThreadNum
    void setReusePort(bool on) { reusePort_ = on; }                   // createsocket 之前调用
    void setMaxConnectionNum(uint32_t num) { maxConnections_ = num; } // 0 = 不限
    void setUnpack(const unpack_setting_t* setting);
    void setKeepaliveTimeout(int timeoutMs) { keepaliveTimeoutMs_ = timeoutMs; }

    int start(bool wait_threads_started = true);   // 成功返回 0
    // 停止接受新连接、关闭全部连接、join 自建线程。
    // 不可在自建线程（本服务器的回调）里调用；accept loop 和 worker 都是外部提供时，可以在这些 loop 里调用。
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

    bool inOwnThread() const;

    std::string name_;
    std::unique_ptr<EventLoopThread> acceptorThread_;   // 使用外部 loop 时为空
    EventLoop* acceptorLoop_ = nullptr;
    std::unique_ptr<EventLoopThreadPool> workerPool_;   // 自建 worker 池
    EventLoopThreadPool* externalPool_ = nullptr;
    std::vector<EventLoop*> ioLoops_;
    size_t nextLoop_ = 0;
    std::shared_ptr<Acceptor> acceptor_;
    bool reusePort_ = false;

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
