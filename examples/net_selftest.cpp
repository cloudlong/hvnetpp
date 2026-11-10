// UDP / TCP 集成回归测试（回环地址）：
// reuseport 多 socket 分流、pktinfo、外部 loop / 线程池、sendBatch、GSO/GRO、回调内销毁、端口冲突检测
#include "hvnetpp/hvnetpp.h"
#include "hvnetpp/SocketsOps.h"
#include "rtclog.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace hvnetpp;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

static int g_failures = 0;

#define CHECK(expr)                                                                \
    do {                                                                           \
        if (!(expr)) {                                                             \
            ++g_failures;                                                          \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        }                                                                          \
    } while (0)

template <typename Pred>
static bool waitFor(Pred pred, int timeoutMs = 3000) {
    auto t0 = Clock::now();
    while (!pred()) {
        if (Clock::now() - t0 > milliseconds(timeoutMs)) return false;
        std::this_thread::sleep_for(milliseconds(2));
    }
    return true;
}

// 原始阻塞 UDP socket，模拟远端
struct RawUdp {
    int fd;
    explicit RawUdp(int rcvTimeoutMs = 2000) {
        fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
        struct timeval tv;
        tv.tv_sec = rcvTimeoutMs / 1000;
        tv.tv_usec = (rcvTimeoutMs % 1000) * 1000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    ~RawUdp() { ::close(fd); }
    InetAddress addr() const { return InetAddress(sockets::getLocalAddr(fd)); }
    void sendTo(const std::string& msg, int port) {
        InetAddress dst("127.0.0.1", static_cast<uint16_t>(port));
        ::sendto(fd, msg.data(), msg.size(), 0, dst.getSockAddr(), dst.socklen());
    }
    int recv(char* buf, size_t cap) { return static_cast<int>(::recv(fd, buf, cap, 0)); }
};

static int boundPort(int fd) {
    return InetAddress(sockets::getLocalAddr(fd)).toPort();
}

// 1. 自建 4 线程 + reuseport：多个对端被分到多个 loop，同一对端固定在同一 loop；pktinfo 给出目的 IP
static void testUdpServerReusePort() {
    UdpServer srv("t-reuse");
    srv.setThreadNum(4);
    UdpSocketOptions opts;
    opts.recvPktInfo = true;
    srv.setSocketOptions(opts);
    int fd = srv.createsocket(0, "0.0.0.0");
    CHECK(fd >= 0);
    int port = boundPort(fd);

    std::mutex mu;
    std::map<std::string, std::set<EventLoop*>> loopsByPeer;
    std::atomic<int> badLocal{0};
    srv.onMessage = [&](const UdpPacket& pkt) {
        {
            std::lock_guard<std::mutex> lock(mu);
            loopsByPeer[pkt.peer.toIpPort()].insert(EventLoop::getEventLoopOfCurrentThread());
        }
        if (pkt.local.toIp() != "127.0.0.1" || pkt.local.toPort() != port) ++badLocal;
        srv.sendto(pkt.data, pkt.len, pkt.peer, &pkt.local);
    };
    CHECK(srv.start() == 0);
    CHECK(srv.socketNum() == 4);

    const int kPeers = 32, kMsgs = 5;
    std::vector<std::unique_ptr<RawUdp>> peers;
    for (int i = 0; i < kPeers; ++i) peers.emplace_back(new RawUdp);
    int replies = 0;
    for (int m = 0; m < kMsgs; ++m) {
        for (auto& p : peers) p->sendTo("hello", port);
        for (auto& p : peers) {
            char buf[64];
            if (p->recv(buf, sizeof buf) == 5) ++replies;
        }
    }
    srv.stop();
    CHECK(replies == kPeers * kMsgs);
    CHECK(badLocal == 0);
    std::set<EventLoop*> allLoops;
    bool sticky = true;
    for (auto& kv : loopsByPeer) {
        if (kv.second.size() != 1) sticky = false;
        allLoops.insert(kv.second.begin(), kv.second.end());
    }
    CHECK(sticky);
    CHECK(allLoops.size() > 1);
    printf("testUdpServerReusePort OK (%zu peers over %zu loops)\n", loopsByPeer.size(), allLoops.size());
}

// 2. 默认不开 reuseport：同端口第二次 bind 必须失败（防止误起两个进程）
static void testPortConflictDetected() {
    UdpServer a("t-a"), b("t-b");
    int fd = a.createsocket(0, "127.0.0.1");
    CHECK(fd >= 0);
    CHECK(b.createsocket(boundPort(fd), "127.0.0.1") < 0);

    TcpServer t1("t-tcp1"), t2("t-tcp2");
    int lfd = t1.createsocket(0, "127.0.0.1");
    CHECK(lfd >= 0);
    int tport = boundPort(lfd);
    CHECK(t1.start() == 0);
    CHECK(t2.createsocket(tport, "127.0.0.1") < 0);
    t1.stop();
    printf("testPortConflictDetected OK\n");
}

// 3. 外部线程池 + 外部 loop
static void testUdpExternalLoops() {
    EventLoopThreadPool pool(nullptr, "t-pool");
    pool.setThreadNum(2);
    pool.start();
    CHECK(pool.size() == 2);

    UdpServer srv("t-ext-pool");
    srv.setEventLoopThreadPool(&pool);
    int fd = srv.createsocket(0, "127.0.0.1");
    int port = boundPort(fd);
    std::atomic<int> onPoolLoop{0};
    srv.onMessage = [&](const UdpPacket& pkt) {
        EventLoop* cur = EventLoop::getEventLoopOfCurrentThread();
        if (cur == pool.loop(0) || cur == pool.loop(1)) ++onPoolLoop;
        srv.sendto(pkt.data, pkt.len, pkt.peer);
    };
    CHECK(srv.start() == 0);
    CHECK(srv.socketNum() == 2);

    // UdpClient 挂到池里的一个 loop 上，与服务器共享线程
    UdpClient cli(pool.loop(1), "t-ext-cli");
    CHECK(cli.createsocket(port, "127.0.0.1") >= 0);
    std::atomic<int> got{0};
    std::atomic<bool> onCliLoop{true};
    cli.onMessage = [&](const UdpPacket& pkt) {
        if (EventLoop::getEventLoopOfCurrentThread() != pool.loop(1)) onCliLoop = false;
        if (pkt.len == 4) ++got;
    };
    CHECK(cli.start() == 0);
    for (int i = 0; i < 10; ++i) cli.send("ping", 4);
    CHECK(waitFor([&] { return got.load() == 10; }));
    CHECK(onCliLoop);
    CHECK(onPoolLoop == 10);
    // 外部 loop 模式下可以在 loop 线程里 stop
    pool.loop(1)->runInLoopAndWait([&] { cli.stop(); });
    srv.stop();
    pool.stop();
    printf("testUdpExternalLoops OK\n");
}

// 4. sendBatch + GSO：一项 6 段的 GSO 数据，对端收到 6 个独立数据报；GRO 开启时库内拆段后也是 6 个
static void testSendBatchAndGso(bool gro) {
    EventLoopThread t;
    EventLoop* loop = t.startLoop();
    std::vector<size_t> sizes;
    std::mutex mu;
    std::unique_ptr<UdpSocket> rx;
    loop->runInLoopAndWait([&] {
        rx.reset(new UdpSocket(loop, "t-rx"));
        UdpSocketOptions o;
        o.gro = gro;
        rx->setOptions(o);
        CHECK(rx->bind(InetAddress("127.0.0.1", 0)));
        rx->setMessageCallback([&](const UdpPacket& pkt) {
            std::lock_guard<std::mutex> lock(mu);
            sizes.push_back(pkt.len);
        });
        rx->startReading();
    });
    InetAddress dst = rx->localAddress();

    UdpSocket tx(loop, "t-tx");
    CHECK(tx.bind(InetAddress("127.0.0.1", 0)));
    std::vector<char> gsoBuf(5 * 100 + 50, 'g');
    std::string a = "aaaa", b = "bbbbbbbb";
    UdpSendItem items[3];
    items[0].data = a.data();
    items[0].len = a.size();
    items[0].peer = &dst;
    items[1].data = gsoBuf.data();
    items[1].len = gsoBuf.size();
    items[1].peer = &dst;
    items[1].gsoSize = 100;
    items[2].data = b.data();
    items[2].len = b.size();
    items[2].peer = &dst;
    CHECK(tx.sendBatch(items, 3) == 3);
    CHECK(waitFor([&] {
        std::lock_guard<std::mutex> lock(mu);
        return sizes.size() >= 8;
    }));
    std::this_thread::sleep_for(milliseconds(20));
    {
        std::lock_guard<std::mutex> lock(mu);
        std::vector<size_t> expect{4, 100, 100, 100, 100, 100, 50, 8};
        CHECK(sizes == expect);
    }
    loop->runInLoopAndWait([&] { rx.reset(); });
    printf("testSendBatchAndGso(gro=%d) OK\n", gro ? 1 : 0);
}

// 5. 回调里销毁 UdpSocket 本身（ASan 下验证无 use-after-free）
static void testDestroyInCallback() {
    EventLoopThread t;
    EventLoop* loop = t.startLoop();
    std::unique_ptr<UdpSocket> sock;
    std::atomic<int> calls{0};
    loop->runInLoopAndWait([&] {
        sock.reset(new UdpSocket(loop, "t-self-destroy"));
        CHECK(sock->bind(InetAddress("127.0.0.1", 0)));
        sock->setMessageCallback([&](const UdpPacket&) {
            ++calls;
            sock.reset();
        });
        sock->startReading();
    });
    InetAddress dst = sock->localAddress();
    RawUdp raw;
    for (int i = 0; i < 20; ++i) {
        ::sendto(raw.fd, "x", 1, 0, dst.getSockAddr(), dst.socklen());
    }
    CHECK(waitFor([&] { return calls.load() >= 1; }));
    std::this_thread::sleep_for(milliseconds(20));
    loop->runInLoopAndWait([] {});
    CHECK(calls.load() == 1);
    printf("testDestroyInCallback OK\n");
}

// 5b. 回调里销毁 socket 之后继续访问闭包的捕获变量。闭包刚好 16 字节、可平凡拷贝，
//     会内联存放在 std::function 里——回调若直接存在 UdpSocket 成员中，这里就是 heap-use-after-free
struct SelfDestroyCtx {
    std::unique_ptr<UdpSocket> sock;
    std::atomic<int> calls{0};
    std::atomic<int> bad{0};
};

static void testCaptureAliveAfterDestroy() {
    EventLoopThread t;
    EventLoop* loop = t.startLoop();
    SelfDestroyCtx ctx;
    loop->runInLoopAndWait([&] {
        ctx.sock.reset(new UdpSocket(loop, "t-capture"));
        CHECK(ctx.sock->bind(InetAddress("127.0.0.1", 0)));
        SelfDestroyCtx* c = &ctx;
        int marker = 0x5a5a;
        ctx.sock->setMessageCallback([c, marker](const UdpPacket&) {
            ++c->calls;
            c->sock.reset();
            if (marker != 0x5a5a) ++c->bad;   // 读取已析构对象里的捕获变量会被 ASan 抓到
        });
        ctx.sock->startReading();
    });
    InetAddress dst = ctx.sock->localAddress();
    RawUdp raw;
    ::sendto(raw.fd, "x", 1, 0, dst.getSockAddr(), dst.socklen());
    CHECK(waitFor([&] { return ctx.calls.load() == 1; }));
    loop->runInLoopAndWait([] {});
    CHECK(ctx.bad.load() == 0);
    printf("testCaptureAliveAfterDestroy OK\n");
}

// 5c. startReading/stopReading 之后可以在任意线程析构（原来 Channel 仍登记在 Poller 里，会 abort）
static void testDestroyOffThreadAfterStop() {
    EventLoopThread t;
    EventLoop* loop = t.startLoop();
    std::unique_ptr<UdpSocket> sock(new UdpSocket(loop, "t-offthread"));
    CHECK(sock->bind(InetAddress("127.0.0.1", 0)));
    loop->runInLoopAndWait([&] {
        sock->startReading();
        sock->stopReading();
    });
    sock.reset();   // 主线程析构
    printf("testDestroyOffThreadAfterStop OK\n");
}

// 6. TCP：服务器 accept 在外部 loop、连接分到外部线程池；客户端挂外部 loop 并在自己的回调里 stop
static void testTcpExternalLoops() {
    EventLoopThread acceptThread(EventLoopThread::ThreadInitCallback(), "t-accept");
    EventLoop* acceptLoop = acceptThread.startLoop();
    EventLoopThreadPool workers(nullptr, "t-workers");
    workers.setThreadNum(2);
    workers.start();

    TcpServer srv(acceptLoop, "t-tcp");
    srv.setEventLoopThreadPool(&workers);
    int lfd = srv.createsocket(0, "127.0.0.1");
    CHECK(lfd >= 0);
    int port = boundPort(lfd);
    std::atomic<bool> onWorker{true};
    srv.onMessage = [&](const SocketChannelPtr& ch, Buffer* buf) {
        EventLoop* cur = EventLoop::getEventLoopOfCurrentThread();
        if (cur != workers.loop(0) && cur != workers.loop(1)) onWorker = false;
        ch->write(buf);
    };
    CHECK(srv.start() == 0);

    EventLoopThread cliThread(EventLoopThread::ThreadInitCallback(), "t-cli");
    EventLoop* cliLoop = cliThread.startLoop();
    TcpClient cli(cliLoop, "t-cli");
    CHECK(cli.createsocket(port, "127.0.0.1") == 0);
    std::atomic<bool> echoed{false};
    cli.onConnection = [](const SocketChannelPtr& ch) {
        if (ch->isConnected()) ch->write(std::string("ping"));
    };
    cli.onMessage = [&](const SocketChannelPtr&, Buffer* buf) {
        if (buf->retrieveAllAsString() == "ping") {
            echoed = true;
            cli.stop();   // 外部 loop：允许在自己的回调里 stop
        }
    };
    CHECK(cli.start() == 0);
    CHECK(waitFor([&] { return echoed.load(); }));
    CHECK(waitFor([&] { return srv.connectionNum() == 0; }));
    CHECK(onWorker);
    srv.stop();
    workers.stop();
    printf("testTcpExternalLoops OK\n");
}

int main() {
    rtclog_configure("/tmp/net_selftest.log", false);
    testUdpServerReusePort();
    testPortConflictDetected();
    testUdpExternalLoops();
    testSendBatchAndGso(false);
    testSendBatchAndGso(true);
    testDestroyInCallback();
    testCaptureAliveAfterDestroy();
    testDestroyOffThreadAfterStop();
    testTcpExternalLoops();
    if (g_failures) {
        fprintf(stderr, "net_selftest: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("net_selftest: ALL PASSED\n");
    return 0;
}
