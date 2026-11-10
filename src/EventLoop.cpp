#include "hvnetpp/EventLoop.h"
#include "hvnetpp/Channel.h"
#include "hvnetpp/Poller.h"
#include "hvnetpp/TimerQueue.h"
#include "rtclog.h"

#include <cstdlib>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <unistd.h>

using namespace hvnetpp;

namespace {
__thread EventLoop* t_loopInThisThread = nullptr;

const int64_t kPollTimeNs = 10LL * 1000 * 1000 * 1000;   // 无定时器时最长阻塞 10 秒
const int kMaxTasksPerRound = 1024;   // 每轮最多执行的任务数：多核持续投递时也能及时回到 IO

pid_t gettid_() {
    return static_cast<pid_t>(::syscall(SYS_gettid));
}

int createEventfd() {
    int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evtfd < 0) {
        RTCLOG(RTC_FATAL, "Failed in eventfd");
        abort();
    }
    return evtfd;
}

#pragma GCC diagnostic ignored "-Wold-style-cast"
class IgnoreSigPipe {
public:
    IgnoreSigPipe() {
        ::signal(SIGPIPE, SIG_IGN);
    }
};
IgnoreSigPipe initObj;
}

EventLoop::EventLoop()
    : looping_(false),
      quit_(false),
      eventHandling_(false),
      rerunPending_(false),
      threadId_(std::this_thread::get_id()),
      tid_(gettid_()),
      poller_(new Poller(this)),
      timerQueue_(new TimerQueue(this)),
      wakeupFd_(createEventfd()),
      wakeupChannel_(new Channel(this, wakeupFd_)),
      currentActiveChannel_(nullptr),
      wakeupPending_(false) {
    RTCLOG(RTC_DEBUG, "EventLoop created %p in thread %d", this, tid_);
    if (t_loopInThisThread) {
        RTCLOG(RTC_FATAL, "Another EventLoop %p exists in this thread %d", t_loopInThisThread, tid_);
        std::abort();
    } else {
        t_loopInThisThread = this;
    }
    wakeupChannel_->setReadCallback(std::bind(&EventLoop::handleRead, this));
    wakeupChannel_->enableReading();
}

EventLoop::~EventLoop() {
    RTCLOG(RTC_DEBUG, "EventLoop %p of thread %d destructs in thread %d", this, tid_, gettid_());
    // 先销毁定时器：回调捕获对象的析构可能再投递任务（reset 期间 timerQueue_ 已为空，runAt 等会直接忽略）
    timerQueue_.reset();
    // 丢弃未执行的任务：只析构、不执行。析构过程中可能再投递，循环到空为止
    while (MpscNode* node = pendingQueue_.pop()) {
        Task* task = static_cast<Task*>(node);
        task->invoke(task, false);
    }
    wakeupChannel_->disableAll();
    wakeupChannel_->remove();
    ::close(wakeupFd_);
    t_loopInThisThread = nullptr;
}

EventLoop* EventLoop::getEventLoopOfCurrentThread() {
    return t_loopInThisThread;
}

void EventLoop::loop() {
    assertInLoopThread();
    looping_ = true;
    RTCLOG(RTC_TRACE, "EventLoop %p start looping", this);

    while (!quit_.load(std::memory_order_acquire)) {
        activeChannels_.clear();
        int64_t timeoutNs = 0;
        if (!rerunPending_) {
            timeoutNs = timerQueue_->nextTimeoutNs(std::chrono::steady_clock::now(), kPollTimeNs);
        }
        rerunPending_ = false;
        poller_->poll(timeoutNs, &activeChannels_);

        eventHandling_ = true;
        for (Channel* channel : activeChannels_) {
            currentActiveChannel_ = channel;
            channel->handleEvent();
        }
        currentActiveChannel_ = nullptr;
        timerQueue_->processExpired(std::chrono::steady_clock::now());
        eventHandling_ = false;

        doPendingFunctors();
    }

    RTCLOG(RTC_TRACE, "EventLoop %p stop looping", this);
    looping_ = false;
}

void EventLoop::quit() {
    quit_.store(true, std::memory_order_release);
    if (!isInLoopThread()) {
        wakeup();
    }
}

void EventLoop::enqueue(Task* task) {
    pendingQueue_.push(task);
    if (isInLoopThread()) {
        // IO/定时器回调阶段投递的任务会在本轮 doPendingFunctors 执行，无需唤醒；
        // 其余情况（任务里再投递、loop() 启动前投递）只需让下一次 poll 不阻塞
        if (!eventHandling_) {
            rerunPending_ = true;
        }
        return;
    }
    // 唤醒合并：只有 false -> true 的那个生产者写 eventfd。
    // 消费者在取任务之前把标志清回 false，因此清零之后才完成 push 的生产者必然会再写一次，不会丢唤醒。
    if (!wakeupPending_.exchange(true, std::memory_order_acq_rel)) {
        wakeup();
    }
}

void EventLoop::doPendingFunctors() {
    wakeupPending_.exchange(false, std::memory_order_acq_rel);

    // 先摘下最多 kMaxTasksPerRound 个任务串成本地链表（复用节点的 mpscNext，不分配内存）再执行：
    // 执行期间新投递的任务留到下一轮；摘取期间生产者仍在不断投递时也有上限，不会饿死 IO
    MpscNode* first = nullptr;
    MpscNode* last = nullptr;
    int taken = 0;
    while (taken < kMaxTasksPerRound) {
        MpscNode* node = pendingQueue_.pop();
        if (node == nullptr) {
            break;
        }
        ++taken;
        node->mpscNext.store(nullptr, std::memory_order_relaxed);
        if (last) {
            last->mpscNext.store(node, std::memory_order_relaxed);
        } else {
            first = node;
        }
        last = node;
    }
    if (taken == kMaxTasksPerRound) {
        rerunPending_ = true;   // 可能还有剩余任务，下一次 poll 不阻塞
    }
    for (MpscNode* node = first; node != nullptr;) {
        MpscNode* next = node->mpscNext.load(std::memory_order_relaxed);
        Task* task = static_cast<Task*>(node);
        task->invoke(task, true);   // 执行后释放 node
        node = next;
    }
}

void EventLoop::updateChannel(Channel* channel) {
    assertInLoopThread();
    poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel) {
    assertInLoopThread();
    poller_->removeChannel(channel);
}

bool EventLoop::hasChannel(Channel* channel) {
    assertInLoopThread();
    return poller_->hasChannel(channel);
}

void EventLoop::assertInLoopThread() {
    if (!isInLoopThread()) {
        RTCLOG(RTC_FATAL, "EventLoop::assertInLoopThread - Created in thread %d current thread %d", tid_, gettid_());
        std::abort();
    }
}

void EventLoop::wakeup() {
    uint64_t one = 1;
    ssize_t n = ::write(wakeupFd_, &one, sizeof one);
    if (n != sizeof one) {
        RTCLOG(RTC_ERROR, "EventLoop::wakeup() writes %zd bytes instead of 8", n);
    }
}

void EventLoop::handleRead() {
    uint64_t one = 1;
    ssize_t n = ::read(wakeupFd_, &one, sizeof one);
    if (n != sizeof one) {
        RTCLOG(RTC_ERROR, "EventLoop::handleRead() reads %zd bytes instead of 8", n);
    }
}

TimerId EventLoop::runAt(Timestamp time, TimerCallback cb) {
    if (!timerQueue_) {
        return TimerId();   // 正在析构
    }
    return timerQueue_->addTimer(std::move(cb), time, std::chrono::nanoseconds(0));
}

TimerId EventLoop::runAfter(double delaySeconds, TimerCallback cb) {
    return runAfter(std::chrono::duration<double>(delaySeconds), std::move(cb));
}

TimerId EventLoop::runEvery(double intervalSeconds, TimerCallback cb) {
    return runEvery(std::chrono::duration<double>(intervalSeconds), std::move(cb));
}

TimerId EventLoop::runEveryNs(std::chrono::nanoseconds interval, TimerCallback cb) {
    if (!timerQueue_) {
        return TimerId();
    }
    if (interval.count() <= 0) {
        // 周期 <= 0 退化为立即执行一次（与旧实现一致）
        return timerQueue_->addTimer(std::move(cb), std::chrono::steady_clock::now(), std::chrono::nanoseconds(0));
    }
    return timerQueue_->addTimer(std::move(cb), std::chrono::steady_clock::now() + interval, interval);
}

void EventLoop::cancel(TimerId timerId) {
    if (timerQueue_) {
        timerQueue_->cancel(timerId);
    }
}
