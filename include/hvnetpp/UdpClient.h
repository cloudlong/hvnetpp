#pragma once

#include "hvnetpp/EventLoopThread.h"
#include "hvnetpp/InetAddress.h"
#include "hvnetpp/UdpSocket.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace hvnetpp {

class Buffer;
class EventLoop;

// libhv 风格 UDP 客户端：
//   UdpClient cli;
//   cli.createsocket(9999, "127.0.0.1");
//   cli.onMessage = [](const InetAddress& peer, Buffer* buf) { ... };
//   cli.start();
//   cli.send("hello");
class UdpClient {
public:
    explicit UdpClient(std::string name = "UdpClient");
    ~UdpClient();

    UdpClient(const UdpClient&) = delete;
    UdpClient& operator=(const UdpClient&) = delete;

    // 解析并记录服务端地址（支持域名），创建 UDP socket。成功返回 fd，失败返回 -1。
    int createsocket(int port, const char* host = "127.0.0.1");

    // start() 前调用：对 socket 做 ::connect，内核只收 remote 的包且 ICMP 错误可见；send 走 ::send
    void setConnectedUdp(bool on) { connectedUdp_ = on; }

    void start(bool wait_threads_started = true);
    void stop(bool wait_threads_stopped = true);

    // 线程安全；send 发往 createsocket 记录的地址
    int send(const void* data, size_t len);
    int send(const std::string& msg);
    int sendto(const void* data, size_t len, const InetAddress& peer); // 非 connected 模式下可发任意地址

    std::function<void(const InetAddress& peer, Buffer* buf)> onMessage;

    EventLoop* loop() { return loop_; }
    const std::string& name() const { return name_; }
    const InetAddress& remoteAddress() const { return remoteAddr_; }

private:
    std::string name_;
    EventLoopThread loopThread_;
    EventLoop* loop_ = nullptr;
    int fd_ = -1;
    InetAddress remoteAddr_;
    bool hasRemoteAddr_ = false;
    bool connectedUdp_ = false;
    std::unique_ptr<UdpSocket> socket_;   // 在 loop 线程内创建/销毁
    std::atomic<bool> started_{false};
};

} // namespace hvnetpp
