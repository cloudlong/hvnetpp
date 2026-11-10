// 性能基准（E2）：跨线程投递 / UDP 收包 / 定时器
// 用法:
//   net_bench post  [producers=4] [perProducer=1000000]   跨线程 queueInLoop 吞吐，并校验每生产者 FIFO
//   net_bench udp   [senders=2] [size=1200] [sec=3] [threads=1]
//                                                         UdpServer 收包能力（发送端用原始 sendmmsg 打满）
//   net_bench timer [n=200000] [periodic=1000] [sec=3]    定时器增删开销 + 周期定时器 CPU 占用
// 结果受机器、内核和虚拟化影响很大，只用于同一环境下改动前后的对比。
#include "hvnetpp/hvnetpp.h"
#include "rtclog.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <random>
#include <sys/resource.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace hvnetpp;
using Clock = std::chrono::steady_clock;

static double secondsSince(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

static double cpuSeconds() {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

// ---------------- post: 跨线程投递吞吐 + 每生产者 FIFO 校验 ----------------
struct PostState {
    std::vector<int64_t> last;
    int64_t violations = 0;
    int64_t done = 0;
    int64_t total = 0;
    std::promise<void> finished;
};

static int benchPost(int producers, int64_t perProducer) {
    EventLoopThread t;
    EventLoop* loop = t.startLoop();
    PostState st;
    st.last.assign(producers, -1);
    st.total = producers * perProducer;

    auto t0 = Clock::now();
    std::vector<std::thread> ths;
    for (int p = 0; p < producers; ++p) {
        ths.emplace_back([loop, &st, p, perProducer] {
            for (int64_t i = 0; i < perProducer; ++i) {
                loop->queueInLoop([&st, p, i] {
                    if (i != st.last[p] + 1) ++st.violations;
                    st.last[p] = i;
                    if (++st.done == st.total) st.finished.set_value();
                });
            }
        });
    }
    for (auto& th : ths) th.join();
    st.finished.get_future().wait();
    double sec = secondsSince(t0);
    printf("post: producers=%d total=%lld time=%.3fs rate=%.2f Mops/s fifo_violations=%lld\n",
           producers, (long long)st.total, sec, st.total / sec / 1e6, (long long)st.violations);
    t.stop();
    return 0;
}

// ---------------- udp: UdpServer 收包能力 ----------------
static int benchUdp(int senders, int pktSize, double durationSec, int port, int threads) {
    std::atomic<int64_t> received{0};
    UdpServer srv("bench");
    srv.setThreadNum(threads);
    if (srv.createsocket(port, "127.0.0.1") < 0) return 1;
    srv.onMessage = [&received](const UdpPacket&) {
        received.fetch_add(1, std::memory_order_relaxed);
    };
    if (srv.start() != 0) return 1;

    std::atomic<bool> stop{false};
    std::atomic<int64_t> sent{0};
    std::vector<std::thread> ths;
    for (int s = 0; s < senders; ++s) {
        ths.emplace_back([&, s] {
            int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof addr);
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
            const int kBatch = 64;
            std::vector<char> payload(pktSize, 'x');
            struct mmsghdr msgs[kBatch];
            struct iovec iovs[kBatch];
            for (int i = 0; i < kBatch; ++i) {
                iovs[i].iov_base = payload.data();
                iovs[i].iov_len = payload.size();
                memset(&msgs[i], 0, sizeof msgs[i]);
                msgs[i].msg_hdr.msg_iov = &iovs[i];
                msgs[i].msg_hdr.msg_iovlen = 1;
            }
            int64_t local = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                int n = ::sendmmsg(fd, msgs, kBatch, 0);
                if (n > 0) local += n;
            }
            sent += local;
            ::close(fd);
        });
    }
    // 预热 0.3s 后计数
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    int64_t r0 = received.load();
    double c0 = cpuSeconds();
    auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::duration<double>(durationSec));
    int64_t r1 = received.load();
    double sec = secondsSince(t0);
    double cpu = cpuSeconds() - c0;
    stop = true;
    for (auto& th : ths) th.join();
    srv.stop();
    printf("udp: threads=%d senders=%d size=%d recv=%.3f Mpps (%.1f MB/s) send_total=%lld process_cpu=%.2f cores\n",
           threads, senders, pktSize, (r1 - r0) / sec / 1e6, (r1 - r0) * (double)pktSize / sec / 1e6,
           (long long)sent.load(), cpu / sec);
    return 0;
}

// ---------------- timer: 增删开销 + 周期定时器 CPU ----------------
static int benchTimer(int n, int periodic, double periodicSec) {
    EventLoopThread t;
    EventLoop* loop = t.startLoop();
    {
        std::promise<double> p;
        loop->runInLoop([&] {
            std::mt19937 rng(42);
            std::vector<TimerId> ids;
            ids.reserve(n);
            auto t0 = Clock::now();
            for (int i = 0; i < n; ++i) {
                ids.push_back(loop->runAfter(1.0 + (rng() % 10000) / 1000.0, [] {}));
            }
            for (int i = 0; i < n; ++i) loop->cancel(ids[i]);
            p.set_value(secondsSince(t0));
        });
        double sec = p.get_future().get();
        printf("timer: add+cancel n=%d time=%.3fs per_op=%.0f ns\n", n, sec, sec / (2.0 * n) * 1e9);
    }
    {
        // periodic 个 5ms 周期定时器，统计触发次数与 CPU
        std::atomic<int64_t> fires{0};
        std::vector<TimerId> ids(periodic);
        std::promise<void> armed;
        loop->runInLoop([&] {
            for (int i = 0; i < periodic; ++i) {
                ids[i] = loop->runEvery(0.005, [&fires] { fires.fetch_add(1, std::memory_order_relaxed); });
            }
            armed.set_value();
        });
        armed.get_future().wait();
        double c0 = cpuSeconds();
        int64_t f0 = fires.load();
        auto t0 = Clock::now();
        std::this_thread::sleep_for(std::chrono::duration<double>(periodicSec));
        double sec = secondsSince(t0);
        int64_t f1 = fires.load();
        double cpu = cpuSeconds() - c0;
        std::promise<void> cancelled;
        loop->runInLoop([&] {
            for (auto& id : ids) loop->cancel(id);
            cancelled.set_value();
        });
        cancelled.get_future().wait();
        printf("timer: periodic=%d x 5ms fires/s=%.0f (expect %.0f) cpu=%.3f cores ns_per_fire=%.0f\n",
               periodic, (f1 - f0) / sec, periodic / 0.005, cpu / sec, cpu / (f1 - f0) * 1e9);
    }
    t.stop();
    return 0;
}

int main(int argc, char* argv[]) {
    rtclog_configure("/tmp/net_bench.log", false);
    if (argc < 2) {
        fprintf(stderr, "usage: %s post [producers] [perProducer] | udp [senders] [size] [sec] [threads] | timer [n] [periodic] [sec]\n", argv[0]);
        return 1;
    }
    std::string cmd = argv[1];
    if (cmd == "post") {
        return benchPost(argc > 2 ? atoi(argv[2]) : 4, argc > 3 ? atoll(argv[3]) : 1000000);
    } else if (cmd == "udp") {
        return benchUdp(argc > 2 ? atoi(argv[2]) : 2, argc > 3 ? atoi(argv[3]) : 1200,
                        argc > 4 ? atof(argv[4]) : 3.0, 19999, argc > 5 ? atoi(argv[5]) : 1);
    } else if (cmd == "timer") {
        return benchTimer(argc > 2 ? atoi(argv[2]) : 200000, argc > 3 ? atoi(argv[3]) : 1000,
                          argc > 4 ? atof(argv[4]) : 3.0);
    }
    return 1;
}
