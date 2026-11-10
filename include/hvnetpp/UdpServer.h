#pragma once

#include "hvnetpp/InetAddress.h"
#include "hvnetpp/UdpSocket.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace hvnetpp {

class EventLoop;
class EventLoopThreadPool;

// libhv 风格 UDP 服务端：
//   UdpServer srv("media");
//   srv.createsocket(9999);
//   srv.setThreadNum(4);   // 可选：4 个线程各一个 socket，SO_REUSEPORT 绑同一端口，内核按四元组分流
//   srv.onMessage = [&srv](const UdpPacket& pkt) { srv.sendto(pkt.data, pkt.len, pkt.peer); };
//   srv.start();
//
// loop 来源（优先级从高到低）：
//   1. setEventLoopThreadPool(pool)：外部线程池（不拥有），池里每个 loop 一个 socket
//   2. UdpServer(loop)：外部 loop（不拥有），一个 socket
//   3. 默认：自建 setThreadNum(N) 个线程（默认 1），每线程一个 socket
// 多 socket 时同一对端（四元组）固定落在同一个 socket / 线程上，会话状态可以无锁地放在该 loop 里。
// 发送是数据报语义，不缓冲；失败返回 -1（errno 保留）。
class UdpServer {
public:
    explicit UdpServer(std::string name = "UdpServer");
    explicit UdpServer(EventLoop* loop, std::string name = "UdpServer");
    ~UdpServer();

    UdpServer(const UdpServer&) = delete;
    UdpServer& operator=(const UdpServer&) = delete;

    // 创建 UDP socket 并立即 bind（用于尽早发现端口冲突）。成功返回 fd，失败返回 -1。
    // 若 start() 时需要多个 socket 而这个 fd 绑定时没开 SO_REUSEPORT，会关掉重建（返回的 fd 随之失效）。
    int createsocket(int port, const char* host = "0.0.0.0");
    void closesocket();   // 须在 stop() 之后调用

    // 以下配置须在 start() 之前调用
    void setThreadNum(int num) { threadNum_ = num; }
    void setEventLoopThreadPool(EventLoopThreadPool* pool) { externalPool_ = pool; }   // 须已 start()
    void setSocketOptions(const UdpSocketOptions& opts) { options_ = opts; }

    // 成功返回 0，失败返回 -1
    int start(bool wait_threads_started = true);
    // 停止收包并 join 自建线程；fd 保留到析构或 closesocket()，因此与其他线程上并发的 sendto 不会冲突。
    // 不可在自建线程（本服务器的回调）里调用；使用外部 loop/线程池时可以在这些 loop 里调用。
    void stop(bool wait_threads_stopped = true);

    // 线程安全，start() 返回后到 stop() 之前有效。
    // 在某个 socket 所属的 loop 线程里调用时用该 socket 发送（无跨核竞争），否则用第一个 socket。
    // local：指定源 IP（配合 UdpSocketOptions::recvPktInfo 与 pkt.local，多网卡时按收包的目的 IP 回包）
    int sendto(const void* data, size_t len, const InetAddress& peer, const InetAddress* local = nullptr);
    int sendto(const std::string& msg, const InetAddress& peer) { return sendto(msg.data(), msg.size(), peer); }
    // sendmmsg 批量发送（可含 GSO 项）；返回完整发出的项数，一项都没发出返回 -1
    int sendBatch(const UdpSendItem* items, size_t n);

    // 在收包 socket 所属的 loop 线程回调（start() 前设置）
    UdpMessageCallback onMessage;

    // 第 idx 个 socket 所在的 loop；未 start 时返回外部 loop（如有）或 nullptr
    EventLoop* loop(size_t idx = 0) const;
    size_t socketNum() const { return sockets_.size(); }
    const std::string& name() const { return name_; }

private:
    int pickFd() const;
    void closeAll();

    std::string name_;
    EventLoop* externalLoop_ = nullptr;
    EventLoopThreadPool* externalPool_ = nullptr;
    std::unique_ptr<EventLoopThreadPool> ownPool_;
    int threadNum_ = 1;
    UdpSocketOptions options_;

    InetAddress bindAddr_;
    bool bound_ = false;
    bool fdsReusePort_ = false;   // 现有 fd 是否以 SO_REUSEPORT 绑定
    std::vector<int> fds_;        // 本对象拥有，析构 / closesocket 时关闭
    std::vector<EventLoop*> loops_;
    std::vector<std::unique_ptr<UdpSocket>> sockets_;   // 在各自 loop 线程内创建/销毁，不拥有 fd
    std::atomic<bool> started_{false};
};

} // namespace hvnetpp
