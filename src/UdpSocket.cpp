#include "hvnetpp/UdpSocket.h"
#include "UdpIo.h"
#include "hvnetpp/Channel.h"
#include "hvnetpp/EventLoop.h"
#include "hvnetpp/SocketsOps.h"
#include "rtclog.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef SOL_UDP
#define SOL_UDP 17
#endif
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
#ifndef UDP_GRO
#define UDP_GRO 104
#endif

namespace hvnetpp {

namespace {

const int kRecvBatch = 32;              // 每次 recvmmsg 最多取的数据报数
const size_t kRecvSlot = 65536;         // 每个数据报的接收缓冲（GRO 合并后最大 64KB）
const int kRecvBudget = 256;            // 单次可读事件最多处理的数据报数，之后让出给同 loop 的其他 fd（LT 会再次通知）
const size_t kRecvCtrl = 128;           // 每个数据报的控制消息缓冲（PKTINFO + GRO）
const size_t kSendChunk = 64;           // 每次 sendmmsg 最多的消息数
const size_t kSendCtrl = 80;            // 发送控制消息缓冲（PKTINFO + UDP_SEGMENT）
const size_t kGsoMaxSegments = 64;      // 旧内核 UDP_MAX_SEGMENTS
const size_t kGsoMaxBytes = 65000;

bool setIntOpt(int fd, int level, int name, int value) {
    return ::setsockopt(fd, level, name, &value, static_cast<socklen_t>(sizeof value)) == 0;
}

int getIntOpt(int fd, int level, int name) {
    int value = 0;
    socklen_t len = static_cast<socklen_t>(sizeof value);
    if (::getsockopt(fd, level, name, &value, &len) != 0) {
        return -1;
    }
    return value;
}

// 进程级 GSO 能力：0 = 未探测，1 = 可用，-1 = 不可用（旧内核，或发送时遇到 EIO 说明网卡不支持校验和卸载）
std::atomic<int> s_gsoState{0};

bool gsoAvailable(int fd) {
    int state = s_gsoState.load(std::memory_order_relaxed);
    if (state == 0) {
        state = getIntOpt(fd, SOL_UDP, UDP_SEGMENT) >= 0 ? 1 : -1;
        s_gsoState.store(state, std::memory_order_relaxed);
    }
    return state > 0;
}

// 按 socket 地址族写入源地址控制消息，返回占用字节数（0 表示无法指定）
size_t putPktInfo(char* buf, sa_family_t sockFamily, const InetAddress& local) {
    struct cmsghdr* c = reinterpret_cast<struct cmsghdr*>(buf);
    if (sockFamily == AF_INET6) {
        struct in6_pktinfo pi;
        memset(&pi, 0, sizeof pi);
        if (local.family() == AF_INET6) {
            const struct sockaddr_in6* a = sockets::sockaddr_in6_cast(local.getSockAddr());
            pi.ipi6_addr = a->sin6_addr;
            if (IN6_IS_ADDR_LINKLOCAL(&a->sin6_addr)) {
                pi.ipi6_ifindex = a->sin6_scope_id;
            }
        } else if (local.family() == AF_INET) {
            // 双栈 socket 上的 IPv4 流量：用 v4-mapped 地址，内核会转成 IPv4 源地址
            const struct sockaddr_in* a = sockets::sockaddr_in_cast(local.getSockAddr());
            pi.ipi6_addr.s6_addr[10] = 0xff;
            pi.ipi6_addr.s6_addr[11] = 0xff;
            memcpy(&pi.ipi6_addr.s6_addr[12], &a->sin_addr, 4);
        } else {
            return 0;
        }
        c->cmsg_level = IPPROTO_IPV6;
        c->cmsg_type = IPV6_PKTINFO;
        c->cmsg_len = CMSG_LEN(sizeof pi);
        memcpy(CMSG_DATA(c), &pi, sizeof pi);
        return CMSG_SPACE(sizeof pi);
    }
    if (local.family() != AF_INET) {
        return 0;
    }
    struct in_pktinfo pi;
    memset(&pi, 0, sizeof pi);
    pi.ipi_spec_dst = sockets::sockaddr_in_cast(local.getSockAddr())->sin_addr;
    c->cmsg_level = IPPROTO_IP;
    c->cmsg_type = IP_PKTINFO;
    c->cmsg_len = CMSG_LEN(sizeof pi);
    memcpy(CMSG_DATA(c), &pi, sizeof pi);
    return CMSG_SPACE(sizeof pi);
}

size_t putGso(char* buf, uint16_t segmentSize) {
    struct cmsghdr* c = reinterpret_cast<struct cmsghdr*>(buf);
    c->cmsg_level = SOL_UDP;
    c->cmsg_type = UDP_SEGMENT;
    c->cmsg_len = CMSG_LEN(sizeof segmentSize);
    memcpy(CMSG_DATA(c), &segmentSize, sizeof segmentSize);
    return CMSG_SPACE(sizeof segmentSize);
}

void setBufferSize(int fd, int forceName, int name, int bytes, const char* what) {
    if (!setIntOpt(fd, SOL_SOCKET, forceName, bytes) && !setIntOpt(fd, SOL_SOCKET, name, bytes)) {
        RTCLOG(RTC_WARN, "UDP fd=%d 设置 %s=%d 失败: %s", fd, what, bytes, strerror(errno));
        return;
    }
    // 内核返回的是翻倍后的值；明显偏小说明被 net.core.[rw]mem_max 截断了
    const int actual = getIntOpt(fd, SOL_SOCKET, name);
    if (actual >= 0 && actual < bytes) {
        RTCLOG(RTC_WARN, "UDP fd=%d %s 请求 %d 实际 %d（受 net.core.%s 限制，可调大 sysctl 或授予 CAP_NET_ADMIN）",
               fd, what, bytes, actual, name == SO_RCVBUF ? "rmem_max" : "wmem_max");
    }
}

} // namespace

// ============================================================================
// udpio：fd 级操作
// ============================================================================
namespace udpio {

void applyOptions(int fd, sa_family_t family, const UdpSocketOptions& o) {
    const bool v6 = family == AF_INET6;
    if (o.recvBufferSize > 0) {
        setBufferSize(fd, SO_RCVBUFFORCE, SO_RCVBUF, o.recvBufferSize, "SO_RCVBUF");
    }
    if (o.sendBufferSize > 0) {
        setBufferSize(fd, SO_SNDBUFFORCE, SO_SNDBUF, o.sendBufferSize, "SO_SNDBUF");
    }
    if (o.tos >= 0) {
        bool ok = v6 ? setIntOpt(fd, IPPROTO_IPV6, IPV6_TCLASS, o.tos) : setIntOpt(fd, IPPROTO_IP, IP_TOS, o.tos);
        if (v6) {
            setIntOpt(fd, IPPROTO_IP, IP_TOS, o.tos);   // 双栈 socket 上的 IPv4 流量
        }
        if (!ok) {
            RTCLOG(RTC_WARN, "UDP fd=%d 设置 TOS=%d 失败: %s", fd, o.tos, strerror(errno));
        }
    }
    if (o.mtuDiscover >= 0) {
        bool ok = v6 ? setIntOpt(fd, IPPROTO_IPV6, IPV6_MTU_DISCOVER, o.mtuDiscover)
                     : setIntOpt(fd, IPPROTO_IP, IP_MTU_DISCOVER, o.mtuDiscover);
        if (v6) {
            setIntOpt(fd, IPPROTO_IP, IP_MTU_DISCOVER, o.mtuDiscover);
        }
        if (!ok) {
            RTCLOG(RTC_WARN, "UDP fd=%d 设置 MTU_DISCOVER=%d 失败: %s", fd, o.mtuDiscover, strerror(errno));
        }
    }
    if (o.recvPktInfo) {
        bool ok = v6 ? setIntOpt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, 1) : setIntOpt(fd, IPPROTO_IP, IP_PKTINFO, 1);
        if (!ok) {
            RTCLOG(RTC_WARN, "UDP fd=%d 开启 PKTINFO 失败: %s", fd, strerror(errno));
        }
    }
    if (o.gro && !setIntOpt(fd, SOL_UDP, UDP_GRO, 1)) {
        RTCLOG(RTC_WARN, "UDP fd=%d 开启 UDP_GRO 失败（需 Linux 5.0+）: %s", fd, strerror(errno));
    }
}

int createBound(const InetAddress& addr, const UdpSocketOptions& opts, bool reusePort) {
    int fd = sockets::createNonblockingUdp(addr.family());
    if (fd < 0) {
        return -1;
    }
    if (reusePort && !setIntOpt(fd, SOL_SOCKET, SO_REUSEPORT, 1)) {
        int err = errno;
        ::close(fd);
        errno = err;
        return -1;
    }
    applyOptions(fd, addr.family(), opts);
    if (::bind(fd, addr.getSockAddr(), addr.socklen()) < 0) {
        int err = errno;
        ::close(fd);
        errno = err;
        return -1;
    }
    return fd;
}

ssize_t sendTo(int fd, sa_family_t family, const void* data, size_t len,
               const InetAddress* peer, const InetAddress* local) {
    if (local == nullptr) {
        if (peer != nullptr) {
            return ::sendto(fd, data, len, 0, peer->getSockAddr(), peer->socklen());
        }
        return ::send(fd, data, len, 0);
    }
    struct iovec iov;
    iov.iov_base = const_cast<void*>(data);
    iov.iov_len = len;
    alignas(struct cmsghdr) char ctrl[kSendCtrl];
    memset(ctrl, 0, sizeof ctrl);
    struct msghdr h;
    memset(&h, 0, sizeof h);
    if (peer != nullptr) {
        h.msg_name = const_cast<struct sockaddr*>(peer->getSockAddr());
        h.msg_namelen = peer->socklen();
    }
    h.msg_iov = &iov;
    h.msg_iovlen = 1;
    const size_t clen = putPktInfo(ctrl, family, *local);
    if (clen > 0) {
        h.msg_control = ctrl;
        h.msg_controllen = clen;
    }
    return ::sendmsg(fd, &h, 0);
}

int sendBatch(int fd, sa_family_t family, const UdpSendItem* items, size_t n) {
    struct mmsghdr msgs[kSendChunk];
    struct iovec iovs[kSendChunk];
    alignas(struct cmsghdr) char ctrl[kSendChunk][kSendCtrl];
    size_t msgItem[kSendChunk];     // 第 k 条消息属于哪一项
    size_t msgOffset[kSendChunk];   // 在该项 data 中的起始偏移（拆段发送时非 0）
    bool msgLast[kSendChunk];       // 是否该项的最后一条消息

    size_t item = 0;                // 下一条待发消息的位置
    size_t offset = 0;
    size_t completed = 0;
    size_t noGsoItem = static_cast<size_t>(-1);   // GSO 被内核拒绝（EINVAL）的项，改为拆段

    while (item < n) {
        size_t k = 0;
        size_t it = item;
        size_t off = offset;
        bool firstIsGso = false;
        while (k < kSendChunk && it < n) {
            const UdpSendItem& si = items[it];
            const size_t seg = si.gsoSize;
            const bool multiSeg = seg > 0 && si.len > seg;
            const bool useGso = multiSeg && off == 0 && it != noGsoItem &&
                                (si.len + seg - 1) / seg <= kGsoMaxSegments && si.len <= kGsoMaxBytes &&
                                gsoAvailable(fd);
            const size_t msgLen = (multiSeg && !useGso) ? std::min(seg, si.len - off) : si.len - off;

            memset(&msgs[k], 0, sizeof msgs[k]);
            iovs[k].iov_base = const_cast<char*>(static_cast<const char*>(si.data) + off);
            iovs[k].iov_len = msgLen;
            struct msghdr& h = msgs[k].msg_hdr;
            if (si.peer != nullptr) {
                h.msg_name = const_cast<struct sockaddr*>(si.peer->getSockAddr());
                h.msg_namelen = si.peer->socklen();
            }
            h.msg_iov = &iovs[k];
            h.msg_iovlen = 1;
            size_t clen = 0;
            if (si.local != nullptr || useGso) {
                memset(ctrl[k], 0, kSendCtrl);
                if (si.local != nullptr) {
                    clen += putPktInfo(ctrl[k], family, *si.local);
                }
                if (useGso) {
                    clen += putGso(ctrl[k] + clen, static_cast<uint16_t>(seg));
                }
            }
            if (clen > 0) {
                h.msg_control = ctrl[k];
                h.msg_controllen = clen;
            }
            if (k == 0) {
                firstIsGso = useGso;
            }
            msgItem[k] = it;
            msgOffset[k] = off;
            off += msgLen;
            if (off >= si.len) {
                msgLast[k] = true;
                ++it;
                off = 0;
            } else {
                msgLast[k] = false;
            }
            ++k;
        }

        const int sent = ::sendmmsg(fd, msgs, static_cast<unsigned int>(k), 0);
        if (sent < 0) {
            const int err = errno;
            if (err == EINTR) {
                continue;
            }
            if (firstIsGso && (err == EIO || err == EINVAL)) {
                if (err == EIO) {
                    s_gsoState.store(-1, std::memory_order_relaxed);
                    RTCLOG(RTC_WARN, "UDP GSO 发送返回 EIO（网卡不支持校验和卸载？），改为逐个数据报发送");
                } else {
                    noGsoItem = item;   // 段长超过路径 MTU 等，本项改为拆段
                }
                continue;
            }
            if (completed == 0) {
                errno = err;
                return -1;
            }
            errno = err;
            return static_cast<int>(completed);
        }
        for (int i = 0; i < sent; ++i) {
            if (msgLast[i]) {
                ++completed;
            }
        }
        if (static_cast<size_t>(sent) < k) {
            // 部分发送：从第一条未发出的消息继续（下一次调用会返回具体错误或继续发送）
            item = msgItem[sent];
            offset = msgOffset[sent];
        } else {
            item = it;
            offset = off;
        }
    }
    return static_cast<int>(completed);
}

} // namespace udpio

// ============================================================================
// UdpSocket
// ============================================================================

// 每个 loop 线程一份的收包缓冲：回调里拿到的数据指向这里，因此所有挂在同一 loop 上的 socket 可以共用
struct UdpSocket::RecvBatch {
    struct mmsghdr msgs[kRecvBatch];
    struct iovec iovs[kRecvBatch];
    InetAddress peers[kRecvBatch];
    alignas(struct cmsghdr) char ctrl[kRecvBatch][kRecvCtrl];
    std::unique_ptr<char[]> data;   // kRecvBatch * kRecvSlot，按需触碰页面，常驻内存只有实际用到的部分

    RecvBatch() : data(new char[kRecvBatch * kRecvSlot]) {
        memset(msgs, 0, sizeof msgs);
        for (int i = 0; i < kRecvBatch; ++i) {
            iovs[i].iov_base = data.get() + static_cast<size_t>(i) * kRecvSlot;
            iovs[i].iov_len = kRecvSlot;
        }
    }

    static RecvBatch& forThisThread() {
        thread_local std::unique_ptr<RecvBatch> batch;
        if (!batch) {
            batch.reset(new RecvBatch);
        }
        return *batch;
    }
};

UdpSocket::UdpSocket(EventLoop* loop, std::string name)
    : loop_(loop),
      name_(std::move(name)),
      family_(AF_UNSPEC),
      sockfd_(-1),
      ownsFd_(true),
      alive_(std::make_shared<bool>(true)) {
}

UdpSocket::UdpSocket(EventLoop* loop, std::string name, int sockfd, sa_family_t family, bool ownsFd)
    : loop_(loop),
      name_(std::move(name)),
      family_(family),
      sockfd_(sockfd),
      ownsFd_(ownsFd),
      alive_(std::make_shared<bool>(true)) {
    setupChannel();
    refreshLocalAddr();
}

UdpSocket::~UdpSocket() {
    alive_.reset();
    std::shared_ptr<Channel> channel = std::move(channel_);
    if (channel) {
        channel->disableAll();   // 从未注册过时是空操作，不触碰 loop
        channel->remove();
        if (loop_->isInLoopThread()) {
            // 可能正处于该 Channel 的事件回调、甚至用户回调本身之中（回调里销毁了本对象）：
            // Channel 和用户回调都延迟到下一轮再释放，回调返回前仍可安全访问它的捕获变量
            std::shared_ptr<UdpMessageCallback> cb = std::move(messageCallback_);
            loop_->queueInLoop([channel, cb] {});
        }
    }
    if (ownsFd_ && sockfd_ >= 0) {
        sockets::close(sockfd_);
    }
}

void UdpSocket::setupChannel() {
    channel_ = std::make_shared<Channel>(loop_, sockfd_);
    std::weak_ptr<bool> alive(alive_);
    channel_->setReadCallback([this, alive] {
        if (!alive.expired()) {
            handleRead();
        }
    });
    // ICMP 错误等只置 EPOLLERR 不带 EPOLLIN：必须取走 SO_ERROR，否则 LT 模式会一直触发
    channel_->setErrorCallback([this, alive] {
        if (!alive.expired()) {
            handleError();
        }
    });
}

void UdpSocket::refreshLocalAddr() {
    if (sockfd_ >= 0) {
        localAddr_ = InetAddress(sockets::getLocalAddr(sockfd_));
    }
}

void UdpSocket::setOptions(const UdpSocketOptions& opts) {
    options_ = opts;
    if (sockfd_ >= 0) {
        udpio::applyOptions(sockfd_, family_, opts);
    }
}

bool UdpSocket::ensureSocket(sa_family_t family) {
    if (sockfd_ >= 0) {
        if (family_ != family) {
            RTCLOG(RTC_ERROR, "UdpSocket[%s] 地址族不匹配: socket=%d 请求=%d", name_.c_str(), family_, family);
            errno = EAFNOSUPPORT;
            return false;
        }
        return true;
    }
    int fd = sockets::createNonblockingUdp(family);
    if (fd < 0) {
        RTCLOG(RTC_ERROR, "UdpSocket[%s] 创建 socket 失败: %s", name_.c_str(), strerror(errno));
        return false;
    }
    sockfd_ = fd;
    family_ = family;
    ownsFd_ = true;
    udpio::applyOptions(fd, family, options_);
    setupChannel();
    return true;
}

bool UdpSocket::bind(const InetAddress& addr) {
    if (sockfd_ < 0) {
        int fd = udpio::createBound(addr, options_, options_.reusePort);
        if (fd < 0) {
            int err = errno;
            RTCLOG(RTC_ERROR, "UdpSocket[%s] bind %s 失败: %s", name_.c_str(), addr.toIpPort().c_str(), strerror(err));
            errno = err;
            return false;
        }
        sockfd_ = fd;
        family_ = addr.family();
        ownsFd_ = true;
        setupChannel();
    } else {
        if (family_ != addr.family()) {
            errno = EAFNOSUPPORT;
            return false;
        }
        if (options_.reusePort) {
            setIntOpt(sockfd_, SOL_SOCKET, SO_REUSEPORT, 1);
        }
        if (::bind(sockfd_, addr.getSockAddr(), addr.socklen()) < 0) {
            int err = errno;
            RTCLOG(RTC_ERROR, "UdpSocket[%s] bind %s 失败: %s", name_.c_str(), addr.toIpPort().c_str(), strerror(err));
            errno = err;
            return false;
        }
    }
    refreshLocalAddr();
    return true;
}

bool UdpSocket::connect(const InetAddress& peer) {
    if (!ensureSocket(peer.family())) {
        return false;
    }
    if (::connect(sockfd_, peer.getSockAddr(), peer.socklen()) < 0) {
        int err = errno;
        RTCLOG(RTC_ERROR, "UdpSocket[%s] connect %s 失败: %s", name_.c_str(), peer.toIpPort().c_str(), strerror(err));
        errno = err;
        return false;
    }
    refreshLocalAddr();
    return true;
}

void UdpSocket::startReading() {
    loop_->assertInLoopThread();
    if (channel_ && !reading_) {
        reading_ = true;
        channel_->enableReading();
    }
}

void UdpSocket::stopReading() {
    loop_->assertInLoopThread();
    if (channel_ && reading_) {
        reading_ = false;
        // 从 Poller 中摘除，之后可以在任意线程析构
        channel_->disableAll();
        channel_->remove();
    }
}

ssize_t UdpSocket::sendTo(const void* data, size_t len, const InetAddress& peer, const InetAddress* local) {
    if (!ensureSocket(peer.family())) {
        return -1;
    }
    return udpio::sendTo(sockfd_, family_, data, len, &peer, local);
}

ssize_t UdpSocket::send(const void* data, size_t len) {
    if (sockfd_ < 0) {
        errno = ENOTCONN;
        return -1;
    }
    return udpio::sendTo(sockfd_, family_, data, len, nullptr, nullptr);
}

int UdpSocket::sendBatch(const UdpSendItem* items, size_t n) {
    if (n == 0) {
        return 0;
    }
    if (sockfd_ < 0) {
        if (items[0].peer == nullptr || !ensureSocket(items[0].peer->family())) {
            errno = ENOTCONN;
            return -1;
        }
    }
    return udpio::sendBatch(sockfd_, family_, items, n);
}

void UdpSocket::handleRead() {
    RecvBatch& b = RecvBatch::forThisThread();
    std::weak_ptr<bool> alive(alive_);
    UdpPacket pkt;
    int budget = kRecvBudget;
    while (budget > 0) {
        const int want = std::min(kRecvBatch, budget);
        for (int i = 0; i < want; ++i) {
            struct msghdr& h = b.msgs[i].msg_hdr;
            h.msg_name = &b.peers[i];
            h.msg_namelen = static_cast<socklen_t>(sizeof(InetAddress));
            h.msg_iov = &b.iovs[i];
            h.msg_iovlen = 1;
            h.msg_control = b.ctrl[i];
            h.msg_controllen = kRecvCtrl;
            h.msg_flags = 0;
            b.msgs[i].msg_len = 0;
        }
        const int n = ::recvmmsg(sockfd_, b.msgs, static_cast<unsigned int>(want), MSG_DONTWAIT, nullptr);
        if (n <= 0) {
            if (n < 0) {
                handleRecvError(errno);
            }
            break;
        }
        pkt.recvTime = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) {
            if (!deliver(b, i, pkt, alive)) {
                return;   // 回调中销毁了本对象
            }
        }
        if (!reading_) {
            return;       // 回调中 stopReading()
        }
        budget -= n;
        if (n < want) {
            break;        // 已读空
        }
    }
}

bool UdpSocket::deliver(RecvBatch& b, int idx, UdpPacket& pkt, const std::weak_ptr<bool>& alive) {
    struct msghdr& h = b.msgs[idx].msg_hdr;
    const size_t len = b.msgs[idx].msg_len;
    if (h.msg_flags & MSG_TRUNC) {
        RTCLOG_EVERY_MS(RTC_WARN, 1000, "UdpSocket[%s] 数据报被截断，丢弃", name_.c_str());
        return true;
    }
    pkt.peer = b.peers[idx];
    pkt.local = localAddr_;
    size_t gsoSize = 0;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&h); c != nullptr; c = CMSG_NXTHDR(&h, c)) {
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
            struct in_pktinfo pi;
            memcpy(&pi, CMSG_DATA(c), sizeof pi);
            struct sockaddr_in sa;
            memset(&sa, 0, sizeof sa);
            sa.sin_family = AF_INET;
            sa.sin_port = localAddr_.portNetEndian();
            sa.sin_addr = pi.ipi_addr;   // IP 头里的目的地址
            pkt.local = InetAddress(sa);
        } else if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_PKTINFO) {
            struct in6_pktinfo pi;
            memcpy(&pi, CMSG_DATA(c), sizeof pi);
            struct sockaddr_in6 sa;
            memset(&sa, 0, sizeof sa);
            sa.sin6_family = AF_INET6;
            sa.sin6_port = localAddr_.portNetEndian();
            sa.sin6_addr = pi.ipi6_addr;
            if (IN6_IS_ADDR_LINKLOCAL(&pi.ipi6_addr)) {
                sa.sin6_scope_id = pi.ipi6_ifindex;
            }
            pkt.local = InetAddress(sa);
        } else if (c->cmsg_level == SOL_UDP && c->cmsg_type == UDP_GRO) {
            int seg = 0;
            memcpy(&seg, CMSG_DATA(c), sizeof seg);
            if (seg > 0) {
                gsoSize = static_cast<size_t>(seg);
            }
        }
    }
    const UdpMessageCallback* cb = messageCallback_.get();
    if (cb == nullptr || !*cb) {
        return true;
    }
    const char* data = static_cast<const char*>(b.iovs[idx].iov_base);
    if (gsoSize > 0 && len > gsoSize) {
        // GRO 合并的多个数据报：按段拆开逐个回调
        for (size_t off = 0; off < len; off += gsoSize) {
            pkt.data = data + off;
            pkt.len = std::min(gsoSize, len - off);
            (*cb)(pkt);
            if (alive.expired()) {
                return false;
            }
        }
        return true;
    }
    pkt.data = data;
    pkt.len = len;
    (*cb)(pkt);
    return !alive.expired();
}

void UdpSocket::handleError() {
    const int err = sockets::getSocketError(sockfd_);   // 读取即清除
    if (err != 0) {
        handleRecvError(err);
    }
}

void UdpSocket::handleRecvError(int err) {
    if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
        return;
    }
    if (err == ECONNREFUSED) {
        RTCLOG_EVERY_MS(RTC_WARN, 1000, "UdpSocket[%s] 对端端口不可达（ICMP）: %s", name_.c_str(), strerror(err));
        return;
    }
    RTCLOG_EVERY_MS(RTC_ERROR, 1000, "UdpSocket[%s] 收包错误: %s", name_.c_str(), strerror(err));
}

} // namespace hvnetpp
