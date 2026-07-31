#pragma once

#include "hvnetpp/Callbacks.h"
#include "hvnetpp/EventLoopThread.h"
#include "hvnetpp/InetAddress.h"
#include "hvnetpp/SocketChannel.h"
#include "hvnetpp/TimerId.h"
#include "hvnetpp/Unpack.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace hvnetpp {

class Connector;
class EventLoop;

// 断线重连策略（与 libhv reconn_setting_t 对齐）
struct reconn_setting_t {
    uint32_t min_delay = 1000;   // ms
    uint32_t max_delay = 60000;  // ms
    uint32_t delay_policy = 2;   // 0=固定 min_delay; 1=线性递增; 2=指数退避
    uint32_t max_retry_cnt = 0;  // 0 = 无限重试
    // 内部状态
    uint32_t cur_retry_cnt = 0;
    uint32_t cur_delay = 0;
};

// 计算并推进下一次重连延迟（ms）
uint32_t reconn_setting_calc_delay(reconn_setting_t* reconn);
void reconn_setting_reset(reconn_setting_t* reconn);

// libhv evpp 风格 TCP 客户端：
//   TcpClient cli;
//   cli.createsocket(1234, "127.0.0.1");
//   cli.onConnection = [](const SocketChannelPtr& ch) { ... };
//   cli.onMessage    = [](const SocketChannelPtr& ch, Buffer* buf) { ... };
//   reconn_setting_t reconn; cli.setReconnect(&reconn);
//   cli.start();
// 客户端自带一个 loop 线程；连接 fd 每次尝试时新建。
class TcpClient {
public:
    explicit TcpClient(std::string name = "TcpClient");
    ~TcpClient();

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    // 解析并记录服务端地址（支持域名）。成功返回 0，失败返回 -1。
    int createsocket(int port, const char* host = "127.0.0.1");

    // 以下配置须在 start() 之前调用
    void setConnectTimeout(int timeoutMs) { connectTimeoutMs_ = timeoutMs; }
    void setReconnect(const reconn_setting_t* setting);   // nullptr 关闭重连
    void setUnpack(const unpack_setting_t* setting);
    void setKeepaliveTimeout(int timeoutMs) { keepaliveTimeoutMs_ = timeoutMs; }
    void setHeartbeat(int intervalMs, std::function<void()> fn);

    void start(bool wait_threads_started = true);
    // 断开连接、停止重连、join loop 线程。不可在本客户端的回调里调用。
    void stop(bool wait_threads_stopped = true);

    // 用户回调（start() 前设置）
    ConnectionCallback onConnection;
    MessageCallback onMessage;
    WriteCompleteCallback onWriteComplete;
    std::function<void()> onConnectFailed;   // 每次连接尝试失败时回调

    // 以下接口线程安全
    bool isConnected() const;
    int send(const void* data, size_t len);
    int send(const std::string& msg);
    SocketChannelPtr channel() const;        // 可能为空

    EventLoop* loop() { return loop_; }
    const std::string& name() const { return name_; }
    const InetAddress& remoteAddress() const { return remoteAddr_; }

private:
    void onConnectorSuccess(int sockfd);           // loop 线程
    void onConnectorFail();                        // loop 线程
    void onChannelClose(const SocketChannelPtr&);  // loop 线程
    void scheduleReconnect();                      // loop 线程

    std::string name_;
    EventLoopThread loopThread_;
    EventLoop* loop_ = nullptr;
    std::shared_ptr<Connector> connector_;
    InetAddress remoteAddr_;
    bool hasRemoteAddr_ = false;

    mutable std::mutex channelMutex_;
    SocketChannelPtr channel_;

    int connectTimeoutMs_ = 5000;
    std::unique_ptr<reconn_setting_t> reconn_;
    TimerId reconnectTimer_;
    bool hasReconnectTimer_ = false;

    std::unique_ptr<unpack_setting_t> unpack_;
    int keepaliveTimeoutMs_ = 0;
    int heartbeatIntervalMs_ = 0;
    std::function<void()> heartbeatFn_;

    std::atomic<bool> started_{false};
};

} // namespace hvnetpp
