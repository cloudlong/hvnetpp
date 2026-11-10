#pragma once

#include "hvnetpp/InetAddress.h"
#include "hvnetpp/UdpSocket.h"

#include <atomic>
#include <memory>
#include <string>

namespace hvnetpp {

class EventLoop;
class EventLoopThread;

// libhv 风格 UDP 客户端：
//   UdpClient cli;                 // 或 UdpClient cli(loop) 挂到外部 loop 上（不创建线程）
//   cli.createsocket(9999, "127.0.0.1");
//   cli.onMessage = [](const UdpPacket& pkt) { ... };
//   cli.start();
//   cli.send("hello");
class UdpClient {
public:
    explicit UdpClient(std::string name = "UdpClient");
    explicit UdpClient(EventLoop* loop, std::string name = "UdpClient");
    ~UdpClient();

    UdpClient(const UdpClient&) = delete;
    UdpClient& operator=(const UdpClient&) = delete;

    // 解析并记录服务端地址（支持域名），创建 UDP socket。成功返回 fd，失败返回 -1。
    int createsocket(int port, const char* host = "127.0.0.1");
    void closesocket();   // 须在 stop() 之后调用

    // start() 前调用：对 socket 做 ::connect，内核只收 remote 的包且 ICMP 错误可见；send 走 ::send
    void setConnectedUdp(bool on) { connectedUdp_ = on; }
    // createsocket() 之前设置；之后设置也会立即对现有 fd 生效（reusePort 除外）
    void setSocketOptions(const UdpSocketOptions& opts);

    int start(bool wait_threads_started = true);   // 成功返回 0
    // 停止收包；使用自建线程时还会 join 线程（此时不可在本客户端的回调里调用）。fd 保留到析构
    void stop(bool wait_threads_stopped = true);

    // 线程安全（createsocket 之后、析构之前）；send 发往 createsocket 记录的地址
    int send(const void* data, size_t len);
    int send(const std::string& msg) { return send(msg.data(), msg.size()); }
    int sendto(const void* data, size_t len, const InetAddress& peer);   // 非 connected 模式下可发任意地址
    int sendBatch(const UdpSendItem* items, size_t n);                   // peer 为空的项发往 remote

    UdpMessageCallback onMessage;   // 在 loop 线程回调（start() 前设置）

    EventLoop* loop() const { return loop_; }
    const std::string& name() const { return name_; }
    const InetAddress& remoteAddress() const { return remoteAddr_; }

private:
    std::string name_;
    std::unique_ptr<EventLoopThread> ownThread_;   // 使用外部 loop 时为空
    EventLoop* loop_ = nullptr;
    int fd_ = -1;
    InetAddress remoteAddr_;
    bool hasRemoteAddr_ = false;
    bool connectedUdp_ = false;
    std::atomic<bool> connected_{false};
    UdpSocketOptions options_;
    std::unique_ptr<UdpSocket> socket_;   // 在 loop 线程内创建/销毁，不拥有 fd
    std::atomic<bool> started_{false};
};

} // namespace hvnetpp
