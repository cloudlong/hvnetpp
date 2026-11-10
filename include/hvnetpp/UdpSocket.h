#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <sys/types.h>

#include "hvnetpp/InetAddress.h"
#include "hvnetpp/Timer.h"

namespace hvnetpp {

class EventLoop;
class Channel;

// 收到的一个 UDP 数据报。
// data 指向库内部的收包缓冲（每个 loop 线程一份，批量 recvmmsg 复用），只在回调期间有效；需要保留请自行拷贝。
struct UdpPacket {
    const char* data = nullptr;
    size_t len = 0;
    InetAddress peer;      // 对端地址
    InetAddress local;     // 本端地址：开启 recvPktInfo 时为数据报的实际目的 IP（端口为绑定端口），否则为 socket 绑定地址
    Timestamp recvTime;    // 收包时间（steady_clock；同一批 recvmmsg 共用一个值）
};

using UdpMessageCallback = std::function<void(const UdpPacket& pkt)>;

// 批量发送的一项
struct UdpSendItem {
    const void* data = nullptr;
    size_t len = 0;
    const InetAddress* peer = nullptr;    // 目的地址；connected socket 可为 nullptr
    const InetAddress* local = nullptr;   // 指定源 IP（IP_PKTINFO / IPV6_PKTINFO），nullptr 由内核选路决定
    // > 0：data 由若干个 gsoSize 字节的段首尾相接而成（最后一段可以更短），通过 UDP GSO 一次交给内核，
    // 对端收到的是多个独立数据报。内核或网卡不支持时自动拆成多个数据报发送。单项最多 64 段、总长不超过 65000 字节。
    uint16_t gsoSize = 0;
};

// socket 选项（各接口都在 start()/bind() 之前设置）
struct UdpSocketOptions {
    int recvBufferSize = 0;    // SO_RCVBUF 字节数，0 = 系统默认。有 CAP_NET_ADMIN 时用 SO_RCVBUFFORCE 突破 rmem_max
    int sendBufferSize = 0;    // SO_SNDBUF，同上
    int tos = -1;              // IP_TOS / IPV6_TCLASS（DSCP << 2，如音频 EF = 46 << 2），-1 = 不设置
    int mtuDiscover = -1;      // IP_MTU_DISCOVER / IPV6_MTU_DISCOVER，取值 IP_PMTUDISC_*，-1 = 不设置
    bool recvPktInfo = false;  // 收包时取得目的 IP（多网卡 bind 通配地址时，用 pkt.local 作为源地址回包）
    bool gro = false;          // UDP_GRO（Linux 5.0+）：内核合并同流数据报，库内按段拆开逐个回调
    bool reusePort = false;    // SO_REUSEPORT，bind 前生效。UdpServer 多 socket 模式会自动开启
};

// UDP socket，可挂在任意 EventLoop 上（SFU 端口池模式可直接为每个 transport 建一个）。
// - 收包：recvmmsg 批量读取，每批最多 32 个，单次可读事件最多处理 256 个后让出给同 loop 的其他 fd
// - 回调里可以安全地销毁本对象或调用 stopReading()
// - 发送是数据报语义，不缓冲；EAGAIN 时直接返回 -1（errno 保留）
class UdpSocket {
public:
    // 不立即创建 fd：bind() 时，或首次发送时按目的地址族创建
    explicit UdpSocket(EventLoop* loop, std::string name = "UdpSocket");
    // 收养一个已创建的非阻塞 UDP fd（可能已 bind/connect）。ownsFd = true 时析构关闭它
    UdpSocket(EventLoop* loop, std::string name, int sockfd, sa_family_t family, bool ownsFd = true);
    // startReading() 之后、stopReading() 之前须在 loop 线程析构（可以在本 socket 的回调里析构）
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // fd 已存在则立即生效（reusePort 只对之后的 bind 有意义）；否则在创建 fd 时生效
    void setOptions(const UdpSocketOptions& opts);
    // 创建 socket（如需要）并 bind。失败返回 false（errno 保留），不会 abort
    bool bind(const InetAddress& addr);
    // connected UDP：内核只收该对端的包，ICMP 错误可见；之后可用 send()
    bool connect(const InetAddress& peer);

    // 须在 loop 线程调用
    void startReading();
    void stopReading();
    bool isReading() const { return reading_; }
    // 在 loop 线程回调；不要在回调执行期间替换回调本身
    void setMessageCallback(UdpMessageCallback cb) {
        messageCallback_ = std::make_shared<UdpMessageCallback>(std::move(cb));
    }

    // 发送：fd 已存在（bind/connect 之后，或收养的 fd）时可在任意线程调用，但不得与析构并发；
    // 未 bind 时首次发送会按目的地址族惰性创建 fd，这一次须与其他调用串行
    ssize_t sendTo(const void* data, size_t len, const InetAddress& peer, const InetAddress* local = nullptr);
    ssize_t send(const void* data, size_t len);   // connected socket
    // sendmmsg 批量发送；返回完整发出的项数（遇 EAGAIN 等错误即停止），一项都没发出返回 -1。
    // 注意：拆段发送的 gsoSize 项可能只发出了一部分段，这样的项不计入返回值，重试会重复发送已发出的段
    int sendBatch(const UdpSendItem* items, size_t n);

    int fd() const { return sockfd_; }
    sa_family_t family() const { return family_; }
    EventLoop* loop() const { return loop_; }
    const std::string& name() const { return name_; }
    const InetAddress& localAddress() const { return localAddr_; }

private:
    struct RecvBatch;

    bool ensureSocket(sa_family_t family);
    void setupChannel();
    void refreshLocalAddr();
    void handleRead();
    void handleError();
    // 返回 false 表示回调中本对象已被销毁
    bool deliver(RecvBatch& batch, int idx, UdpPacket& pkt, const std::weak_ptr<bool>& alive);
    void handleRecvError(int err);

    EventLoop* loop_;
    const std::string name_;
    sa_family_t family_;
    int sockfd_;
    bool ownsFd_;
    bool reading_ = false;
    UdpSocketOptions options_;
    InetAddress localAddr_;
    std::shared_ptr<Channel> channel_;
    std::shared_ptr<bool> alive_;    // 析构时释放，handleRead 据此判断回调里是否销毁了自己
    // 回调放在独立的堆对象里：回调中销毁本对象时，析构把它交给延迟任务，正在执行的闭包及其捕获仍然有效
    // （std::function 的小闭包内联存放，只 move std::function 本身不够）
    std::shared_ptr<UdpMessageCallback> messageCallback_;
};

} // namespace hvnetpp
