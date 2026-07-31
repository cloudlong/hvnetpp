#pragma once

#include "hvnetpp/Buffer.h"
#include "hvnetpp/Callbacks.h"
#include "hvnetpp/InetAddress.h"
#include "hvnetpp/TimerId.h"
#include "hvnetpp/Unpack.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace hvnetpp {

class EventLoop;
class Channel;

// 一条 TCP 连接（libhv evpp 的 SocketChannel 角色）。
// 由 TcpServer/TcpClient 创建；生命周期由 shared_ptr 管理。
// write()/close() 线程安全；其余配置接口须在所属 loop 线程或连接建立前调用。
class SocketChannel : public std::enable_shared_from_this<SocketChannel> {
public:
    enum Status { CONNECTING, CONNECTED, DISCONNECTING, DISCONNECTED };

    using HighWaterMarkCallback = std::function<void(const SocketChannelPtr&, size_t)>;

    SocketChannel(EventLoop* loop, int sockfd,
                  const InetAddress& localAddr, const InetAddress& peerAddr);
    ~SocketChannel();

    uint32_t id() const { return id_; }
    int fd() const { return sockfd_; }
    EventLoop* loop() const { return loop_; }
    Status status() const { return status_.load(std::memory_order_acquire); }
    bool isConnected() const { return status() == CONNECTED; }

    std::string peeraddr() const { return peerAddr_.toIpPort(); }
    std::string localaddr() const { return localAddr_.toIpPort(); }
    const InetAddress& peerAddress() const { return peerAddr_; }
    const InetAddress& localAddress() const { return localAddr_; }

    // ---- 线程安全 ----
    // 返回接受的字节数（异步写，成功入队即返回 len），未连接返回 -1
    int write(const void* data, size_t len);
    int write(const std::string& msg);
    int write(Buffer* buf);
    void close();      // 异步关闭连接
    void shutdown();   // 半关闭写端（发完积压数据后 shutdown(WR)）

    // ---- 配置（loop 线程内或连接建立前调用）----
    void setUnpack(const unpack_setting_t* setting);       // nullptr 关闭拆包
    void setKeepaliveTimeout(int timeoutMs);               // 空闲超过 timeoutMs 关闭连接；0 关闭该功能
    void setHeartbeat(int intervalMs, std::function<void()> fn); // 每 intervalMs 调一次 fn
    void setWriteBufferHighWaterMark(size_t highWaterMark) { highWaterMark_ = highWaterMark; }
    void setHighWaterMarkCallback(const HighWaterMarkCallback& cb) { highWaterMarkCallback_ = cb; }
    void setTcpNoDelay(bool on);
    size_t writeBufferSize() const;   // 近似值，loop 线程内读取才精确

    // ---- 用户上下文 ----
    void setContext(void* ctx) { context_ = ctx; }
    void* context() const { return context_; }
    void setContextPtr(const std::shared_ptr<void>& ctx) { contextPtr_ = ctx; }
    const std::shared_ptr<void>& contextPtr() const { return contextPtr_; }
    template <typename T>
    std::shared_ptr<T> getContextPtr() const { return std::static_pointer_cast<T>(contextPtr_); }

    // ---- 内部使用（TcpServer/TcpClient 调用）----
    void setConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) { writeCompleteCallback_ = cb; }
    void setCloseCallback(const CloseCallback& cb) { closeCallback_ = cb; }
    void connectEstablished();   // loop 线程内：注册事件、起定时器、回调建立
    void connectDestroyed();     // loop 线程内：注销事件、取消定时器
    void forceCloseInLoop();     // loop 线程内：handleClose + connectDestroyed 一步完成（停机路径）

private:
    void handleRead();
    void handleWrite();
    void handleClose();
    void handleError();
    void handleError(int err);
    void handleUnpack();
    void sendInLoop(const std::string& message);
    void sendInLoop(const void* message, size_t len);
    void shutdownInLoop();
    void closeSocket();
    void setStatus(Status s) { status_.store(s, std::memory_order_release); }

    void armKeepaliveTimer(double delaySeconds);   // loop 线程内
    void onKeepaliveTimer();
    void armHeartbeatTimer();                       // loop 线程内
    void cancelTimersInLoop();

    EventLoop* loop_;
    const uint32_t id_;
    std::atomic<Status> status_;

    std::unique_ptr<Channel> channel_;
    int sockfd_;

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
    Buffer packetBuffer_;   // 拆包结果复用缓冲

    std::unique_ptr<unpack_setting_t> unpack_;
    int keepaliveTimeoutMs_ = 0;
    int heartbeatIntervalMs_ = 0;
    std::function<void()> heartbeatFn_;
    std::chrono::steady_clock::time_point lastActivity_;
    TimerId keepaliveTimer_;
    bool hasKeepaliveTimer_ = false;
    TimerId heartbeatTimer_;
    bool hasHeartbeatTimer_ = false;
    bool destroyed_ = false;

    void* context_ = nullptr;
    std::shared_ptr<void> contextPtr_;
};

} // namespace hvnetpp
