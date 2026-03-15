#include "hvnetpp/EventLoop.h"
#include "hvnetpp/Channel.h"
#include "hvnetpp/Poller.h"
#include "hvnetpp/TimerQueue.h"
#include "rtclog.h"

#include <algorithm>
#include <cstdlib>
#include <signal.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <sys/syscall.h>

using namespace hvnetpp;

namespace {
__thread EventLoop* t_loopInThisThread = nullptr;

const int kPollTimeMs = 10000;

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
      callingPendingFunctors_(false),
      threadId_(std::this_thread::get_id()),
      tid_(gettid_()),
      poller_(new Poller(this)),
      timerQueue_(new TimerQueue(this)),
      wakeupFd_(createEventfd()),
      wakeupChannel_(new Channel(this, wakeupFd_)),
      currentActiveChannel_(nullptr),
      pendingQueue_(new PendingQueue(16))
{
    RTCLOG(RTC_DEBUG, "EventLoop created %p in thread %d", this, tid_);
    if (t_loopInThisThread) {
        RTCLOG(RTC_FATAL, "Another EventLoop %p exists in this thread %d", t_loopInThisThread, tid_);
        std::abort();
    } else {
        t_loopInThisThread = this;
    }
    if (!pendingQueue_->isValid()) {
        RTCLOG(RTC_WARN, "EventLoop pending MPSC queue unavailable, falling back to mutex queue");
    }
    wakeupChannel_->setReadCallback(std::bind(&EventLoop::handleRead, this));
    wakeupChannel_->enableReading();
}

EventLoop::~EventLoop() {
    RTCLOG(RTC_DEBUG, "EventLoop %p of thread %d destructs in thread %d", this, tid_, gettid_());
    wakeupChannel_->disableAll();
    wakeupChannel_->remove();
    ::close(wakeupFd_);
    t_loopInThisThread = nullptr;
}

void EventLoop::loop() {
    assertInLoopThread();
    looping_ = true;
    quit_ = false;
    RTCLOG(RTC_TRACE, "EventLoop %p start looping", this);

    while (!quit_) {
        activeChannels_.clear();
        poller_->poll(kPollTimeMs, &activeChannels_);
        
        eventHandling_ = true;
        for (Channel* channel : activeChannels_) {
            currentActiveChannel_ = channel;
            channel->handleEvent();
        }
        currentActiveChannel_ = nullptr;
        eventHandling_ = false;

        doPendingFunctors();
    }

    RTCLOG(RTC_TRACE, "EventLoop %p stop looping", this);
    looping_ = false;
}

void EventLoop::quit() {
    quit_ = true;
    if (!isInLoopThread()) {
        wakeup();
    }
}

void EventLoop::runInLoop(Functor cb) {
    if (isInLoopThread()) {
        cb();
    } else {
        queueInLoop(std::move(cb));
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

void EventLoop::doPendingFunctors() {
    callingPendingFunctors_ = true;

    if (pendingQueue_->isValid()) {
        while (true) {
            PendingQueue::Node* node = pendingQueue_->peek();
            if (!node) {
                break;
            }

            FunctorTask& task = node->data;
            if (task.functorPtr) {
                (*task.functorPtr)();
                delete task.functorPtr;
            }

            pendingQueue_->consume(node);
        }
    }

    std::vector<Functor> pendingFunctors;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingFunctors.swap(pendingFunctors_);
    }
    for (Functor& functor : pendingFunctors) {
        functor();
    }

    callingPendingFunctors_ = false;
}

TimerId EventLoop::runAt(Timestamp time, TimerCallback cb) {
    return timerQueue_->addTimer(std::move(cb), time, 0.0);
}

TimerId EventLoop::runAfter(double delay, TimerCallback cb) {
    Timestamp time(std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int64_t>(delay * 1000)));
    return runAt(time, std::move(cb));
}

TimerId EventLoop::runEvery(double interval, TimerCallback cb) {
    Timestamp time(std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int64_t>(interval * 1000)));
    return timerQueue_->addTimer(std::move(cb), time, interval);
}

void EventLoop::cancel(TimerId timerId) {
    return timerQueue_->cancel(timerId);
}

void EventLoop::queueInLoop(Functor cb) {
    PendingQueue::Node* node = nullptr;
    if (pendingQueue_->isValid()) {
        node = pendingQueue_->reserve();
    }

    if (node) {
        node->data.functorPtr = new Functor(std::move(cb));
        pendingQueue_->commit(node, 1);
    } else {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingFunctors_.emplace_back(std::move(cb));
        RTCLOG(RTC_WARN, "queueInLoop lock-free queue unavailable/full, using fallback queue");
    }

    if (!isInLoopThread() || callingPendingFunctors_) {
        wakeup();
    }
}
