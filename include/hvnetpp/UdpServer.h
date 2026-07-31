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

// libhv 风格 UDP 服务端：
//   UdpServer srv;
//   srv.createsocket(9999);
//   srv.onMessage = [&srv](const InetAddress& peer, Buffer* buf) { srv.sendto(..., peer); };
//   srv.start();
// 说明：数据报语义下发送不做 EAGAIN 缓冲，发送失败返回 -1（errno 保留）。
class UdpServer {
public:
    explicit UdpServer(std::string name = "UdpServer");
    ~UdpServer();

    UdpServer(const UdpServer&) = delete;
    UdpServer& operator=(const UdpServer&) = delete;

    // 创建 UDP socket 并立即 bind。成功返回 fd，失败返回 -1。
    int createsocket(int port, const char* host = "0.0.0.0");

    void start(bool wait_threads_started = true);
    void stop(bool wait_threads_stopped = true);

    // 线程安全
    int sendto(const void* data, size_t len, const InetAddress& peer);
    int sendto(const std::string& msg, const InetAddress& peer);

    std::function<void(const InetAddress& peer, Buffer* buf)> onMessage;

    EventLoop* loop() { return loop_; }
    const std::string& name() const { return name_; }

private:
    std::string name_;
    EventLoopThread loopThread_;
    EventLoop* loop_ = nullptr;
    int fd_ = -1;
    sa_family_t family_;
    std::unique_ptr<UdpSocket> socket_;   // 在 loop 线程内创建/销毁
    std::atomic<bool> started_{false};
};

} // namespace hvnetpp
