// EventLoop 任务队列 / 定时器 / InetAddress 回归测试
#include "hvnetpp/EventLoop.h"
#include "hvnetpp/EventLoopThread.h"
#include "hvnetpp/InetAddress.h"
#include "rtclog.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
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

static double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// 应当发生的事件用轮询等待（sanitizer 构建或高负载下可能慢很多），不依赖固定 sleep
template <typename Pred>
static bool waitFor(EventLoop* loop, Pred pred, int timeoutMs = 3000) {
    auto t0 = Clock::now();
    while (true) {
        bool ok = false;
        loop->runInLoopAndWait([&] { ok = pred(); });
        if (ok) return true;
        if (msSince(t0) > timeoutMs) return false;
        std::this_thread::sleep_for(milliseconds(2));
    }
}

// 多生产者：每个生产者内部严格 FIFO，且一个不丢
static void testMultiProducerFifo() {
    EventLoopThread t;
    EventLoop* loop = t.startLoop();
    const int kProducers = 4;
    const int64_t kPerProducer = 200000;
    std::vector<int64_t> last(kProducers, -1);
    int64_t violations = 0;
    std::atomic<int64_t> done{0};
    std::vector<std::thread> ths;
    for (int p = 0; p < kProducers; ++p) {
        ths.emplace_back([&, p] {
            for (int64_t i = 0; i < kPerProducer; ++i) {
                loop->queueInLoop([&, p, i] {
                    if (i != last[p] + 1) ++violations;
                    last[p] = i;
                    done.fetch_add(1, std::memory_order_release);
                });
            }
        });
    }
    for (auto& th : ths) th.join();
    auto t0 = Clock::now();
    while (done.load(std::memory_order_acquire) < kProducers * kPerProducer && msSince(t0) < 10000) {
        std::this_thread::sleep_for(milliseconds(1));
    }
    loop->runInLoopAndWait([] {});
    CHECK(done.load() == kProducers * kPerProducer);
    CHECK(violations == 0);
    printf("testMultiProducerFifo OK (%lld tasks)\n", static_cast<long long>(done.load()));
}

// 唤醒合并不能丢唤醒：逐个投递并等待执行，任何一次丢失都会卡到 10 秒的 poll 超时
static void testNoLostWakeup() {
    EventLoopThread t;
    EventLoop* loop = t.startLoop();
    const int kRounds = 20000;
    double worstMs = 0;
    for (int i = 0; i < kRounds; ++i) {
        std::atomic<bool> ran{false};
        auto t0 = Clock::now();
        loop->queueInLoop([&ran] { ran.store(true, std::memory_order_release); });
        while (!ran.load(std::memory_order_acquire)) {
            if (msSince(t0) > 2000) break;
        }
        worstMs = std::max(worstMs, msSince(t0));
        if (!ran.load()) {
            // 等它最终执行完，避免悬空引用
            loop->runInLoopAndWait([] {});
            break;
        }
    }
    CHECK(worstMs < 1000);
    printf("testNoLostWakeup OK (worst %.2f ms)\n", worstMs);
}

// 自投递的任务不能饿死定时器与 IO
static void testSelfRepostDoesNotStarve() {
    EventLoop loop;
    std::atomic<bool> stopRepost{false};
    std::function<void()> repost;
    int64_t reposts = 0;
    repost = [&] {
        ++reposts;
        if (!stopRepost.load()) loop.queueInLoop(repost);
    };
    loop.queueInLoop(repost);
    bool timerFired = false;
    loop.runAfter(milliseconds(20), [&] {
        timerFired = true;
        stopRepost = true;
        loop.quit();
    });
    auto t0 = Clock::now();
    loop.loop();
    CHECK(timerFired);
    CHECK(msSince(t0) < 1000);
    printf("testSelfRepostDoesNotStarve OK (%lld reposts in %.1f ms)\n", static_cast<long long>(reposts), msSince(t0));
}

// loop() 启动前在本线程投递的任务应立即执行，而不是等到 poll 超时
static void testQueueBeforeLoop() {
    EventLoop loop;
    bool ran = false;
    loop.queueInLoop([&] {
        ran = true;
        loop.quit();
    });
    auto t0 = Clock::now();
    loop.loop();
    CHECK(ran);
    CHECK(msSince(t0) < 1000);
    printf("testQueueBeforeLoop OK (%.2f ms)\n", msSince(t0));
}

// EventLoop 析构时未执行的任务只析构、不执行
static void testDestroyDropsPendingTasks() {
    std::mutex mu;
    std::condition_variable cv;
    int stage = 0;
    EventLoop* loopPtr = nullptr;
    std::thread th([&] {
        EventLoop loop;
        {
            std::lock_guard<std::mutex> lock(mu);
            loopPtr = &loop;
            stage = 1;
        }
        cv.notify_all();
        loop.loop();
        {
            std::unique_lock<std::mutex> lock(mu);
            stage = 2;
            cv.notify_all();
            cv.wait(lock, [&] { return stage == 3; });
        }
    });
    {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [&] { return stage == 1; });
    }
    loopPtr->quit();
    {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [&] { return stage == 2; });
    }
    auto token = std::make_shared<int>(0);
    std::atomic<int> ran{0};
    for (int i = 0; i < 100; ++i) {
        loopPtr->queueInLoop([token, &ran] { ++ran; });
    }
    CHECK(token.use_count() == 101);
    {
        std::lock_guard<std::mutex> lock(mu);
        stage = 3;
    }
    cv.notify_all();
    th.join();
    CHECK(token.use_count() == 1);
    CHECK(ran.load() == 0);
    printf("testDestroyDropsPendingTasks OK\n");
}

// loop 已退出（但对象尚未析构）时 runInLoopAndWait 应返回 false，而不是永久阻塞
static void testRunAndWaitOnExitedLoop() {
    std::mutex mu;
    std::condition_variable cv;
    int stage = 0;
    std::atomic<int> callers{0};
    EventLoop* loopPtr = nullptr;
    std::thread th([&] {
        EventLoop loop;
        {
            std::lock_guard<std::mutex> lock(mu);
            loopPtr = &loop;
            stage = 1;
        }
        cv.notify_all();
        loop.loop();
        // 等调用方把任务投进来再析构（投递在计数之后几微秒内完成，留足余量）
        while (callers.load() == 0) std::this_thread::sleep_for(milliseconds(1));
        std::this_thread::sleep_for(milliseconds(200));
    });
    {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [&] { return stage == 1; });
    }
    loopPtr->runInLoopAndWait([&] { loopPtr->quit(); });
    bool ran = true;
    bool fRan = false;
    std::thread caller([&] {
        callers = 1;
        ran = loopPtr->runInLoopAndWait([&] { fRan = true; });
    });
    caller.join();
    th.join();
    CHECK(!ran);
    CHECK(!fRan);
    printf("testRunAndWaitOnExitedLoop OK\n");
}

// 多个线程持续投递时，每轮任务数有上限，定时器和 IO 仍能及时处理。
// 定时器要在洪泛开始前于 loop 内注册：跨线程添加本身也是一个任务，会按 FIFO 排在积压任务之后
static void testFloodDoesNotStarveTimers() {
    EventLoopThread t;
    EventLoop* loop = t.startLoop();
    std::atomic<bool> fired{false};
    double firedMs = -1;
    Clock::time_point t0;
    loop->runInLoopAndWait([&] {
        t0 = Clock::now();
        loop->runAfter(milliseconds(100), [&] {
            firedMs = msSince(t0);
            fired = true;
        });
    });
    std::atomic<bool> stop{false};
    std::atomic<int64_t> executed{0};
    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p) {
        producers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed) && !fired.load(std::memory_order_relaxed)) {
                loop->queueInLoop([&executed] { executed.fetch_add(1, std::memory_order_relaxed); });
            }
        });
    }
    auto w0 = Clock::now();
    while (!fired && msSince(w0) < 3000) std::this_thread::sleep_for(milliseconds(1));
    stop = true;
    for (auto& th : producers) th.join();
    // 等积压任务执行完再退出（捕获了栈上变量）
    while (!loop->runInLoopAndWait([] {})) {}
    CHECK(fired);
    CHECK(firedMs >= 100 && firedMs < 300);
    printf("testFloodDoesNotStarveTimers OK (100ms timer fired at %.1f ms during flood, %lld tasks)\n", firedMs,
           static_cast<long long>(executed.load()));
}

static void testTimers() {
    EventLoopThread t;
    EventLoop* loop = t.startLoop();

    // 1. 到期顺序
    {
        std::vector<int> order;
        loop->runInLoopAndWait([&] {
            loop->runAfter(milliseconds(30), [&] { order.push_back(30); });
            loop->runAfter(milliseconds(10), [&] { order.push_back(10); });
            loop->runAfter(milliseconds(20), [&] { order.push_back(20); });
        });
        CHECK(waitFor(loop, [&] { return order.size() == 3; }));
        CHECK((order == std::vector<int>{10, 20, 30}));
    }
    // 2. 跨线程添加后立即取消
    {
        std::atomic<bool> fired{false};
        TimerId id = loop->runAfter(milliseconds(30), [&] { fired = true; });
        loop->cancel(id);
        std::this_thread::sleep_for(milliseconds(60));
        CHECK(!fired);
    }
    // 3. 同一批到期：前一个回调取消后一个，后一个不应执行
    {
        bool secondRan = false;
        TimerId second;
        loop->runInLoopAndWait([&] {
            Timestamp when = Clock::now() + milliseconds(10);
            loop->runAt(when, [&] { loop->cancel(second); });
            second = loop->runAt(when, [&] { secondRan = true; });
        });
        std::this_thread::sleep_for(milliseconds(40));
        loop->runInLoopAndWait([] {});
        CHECK(!secondRan);
    }
    // 4. 周期定时器在回调里取消自己
    {
        int count = 0;
        loop->runInLoopAndWait([&] {
            auto id = std::make_shared<TimerId>();
            *id = loop->runEvery(milliseconds(5), [&, id] {
                if (++count == 3) loop->cancel(*id);
            });
        });
        CHECK(waitFor(loop, [&] { return count >= 3; }));
        std::this_thread::sleep_for(milliseconds(30));   // 取消之后不应再触发
        loop->runInLoopAndWait([] {});
        CHECK(count == 3);
    }
    // 5. 过期的 TimerId 不能误取消复用了同一对象的新定时器
    {
        std::atomic<bool> secondFired{false};
        TimerId first = loop->runAfter(milliseconds(1), [] {});
        std::this_thread::sleep_for(milliseconds(20));
        loop->runInLoopAndWait([&] {
            loop->runAfter(milliseconds(10), [&] { secondFired = true; });
        });
        loop->cancel(first);
        CHECK(waitFor(loop, [&] { return secondFired.load(); }));
    }
    // 6. 不提前触发，且精度明显好于 10ms
    {
        double worstEarly = 0, worstLate = 0;
        for (int i = 0; i < 20; ++i) {
            std::atomic<bool> done{false};
            Clock::time_point firedAt;
            Clock::time_point t0;
            loop->runInLoopAndWait([&] {
                t0 = Clock::now();
                loop->runAfter(milliseconds(3), [&] {
                    firedAt = Clock::now();
                    done = true;
                });
            });
            while (!done) std::this_thread::sleep_for(milliseconds(1));
            double ms = std::chrono::duration<double, std::milli>(firedAt - t0).count();
            worstEarly = std::max(worstEarly, 3.0 - ms);
            worstLate = std::max(worstLate, ms - 3.0);
        }
        CHECK(worstEarly <= 0.0);   // 绝不提前触发
        CHECK(worstLate < 50.0);    // 只防退化到秒级；实际精度见打印（容器内通常 < 1.5ms）
        printf("  timer precision: worst late %.3f ms\n", worstLate);
    }
    // 7. chrono 周期
    {
        std::atomic<int> fires{0};
        TimerId id = loop->runEvery(milliseconds(5), [&] { ++fires; });
        std::this_thread::sleep_for(milliseconds(105));
        loop->cancel(id);
        loop->runInLoopAndWait([] {});
        int n = fires.load();
        CHECK(n >= 5 && n <= 22);   // 上限防止失控；负载高时落后的周期会被跳过而不是补发
        printf("  runEvery(5ms) fired %d times in 105 ms\n", n);
    }
    printf("testTimers OK\n");
}

static void testInetAddress() {
    InetAddress a("1.2.3.4", 5000), b("1.2.3.4", 5000), c("1.2.3.4", 5001), d("1.2.3.5", 5000);
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a != d);
    CHECK(a.hash() == b.hash());
    InetAddress v6a("::1", 5000, true), v6b("::1", 5000, true), v6c("::2", 5000, true);
    CHECK(v6a == v6b);
    CHECK(v6a != v6c);
    CHECK(v6a.hash() == v6b.hash());
    CHECK(v6a != a);
    CHECK(a.socklen() == sizeof(struct sockaddr_in));
    CHECK(v6a.socklen() == sizeof(struct sockaddr_in6));
    std::unordered_map<InetAddress, int> sessions;
    sessions[a] = 1;
    sessions[v6a] = 2;
    CHECK(sessions.count(b) == 1 && sessions[b] == 1);
    CHECK(sessions.count(c) == 0);
    CHECK(sessions[v6b] == 2);
    printf("testInetAddress OK\n");
}

int main() {
    rtclog_configure("/tmp/loop_selftest.log", false);
    testMultiProducerFifo();
    testNoLostWakeup();
    testSelfRepostDoesNotStarve();
    testQueueBeforeLoop();
    testDestroyDropsPendingTasks();
    testRunAndWaitOnExitedLoop();
    testFloodDoesNotStarveTimers();
    testTimers();
    testInetAddress();
    if (g_failures) {
        fprintf(stderr, "loop_selftest: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("loop_selftest: ALL PASSED\n");
    return 0;
}
