#pragma once

#include <functional>
#include <memory>

namespace hvnetpp {

class EventLoop;
class Poller;

class Channel {
public:
    using EventCallback = std::function<void()>;
    using ReadEventCallback = std::function<void()>;

    Channel(EventLoop* loop, int fd);
    ~Channel();

    void handleEvent();
    void tie(const std::shared_ptr<void>& obj);
    void setReadCallback(ReadEventCallback cb) { readCallback_ = std::move(cb); }
    void setWriteCallback(EventCallback cb) { writeCallback_ = std::move(cb); }
    void setCloseCallback(EventCallback cb) { closeCallback_ = std::move(cb); }
    void setErrorCallback(EventCallback cb) { errorCallback_ = std::move(cb); }

    int fd() const { return fd_; }
    bool isNoneEvent() const { return events_ == kNoneEvent; }

    void enableReading() { events_ |= kReadEvent; update(); }
    void disableReading() { events_ &= ~kReadEvent; update(); }
    void enableWriting() { events_ |= kWriteEvent; update(); }
    void disableWriting() { events_ &= ~kWriteEvent; update(); }
    void disableAll() { events_ = kNoneEvent; update(); }
    bool isWriting() const { return events_ & kWriteEvent; }
    bool isReading() const { return events_ & kReadEvent; }

    EventLoop* ownerLoop() { return loop_; }
    void remove();

private:
    friend class Poller;

    void handleEventWithGuard();
    void update();
    int events() const { return events_; }
    void setRevents(int revt) { revents_ = revt; }
    int index() const { return index_; }
    void setIndex(int idx) { index_ = idx; }

    static const int kNoneEvent;
    static const int kReadEvent;
    static const int kWriteEvent;

    EventLoop* loop_;
    const int fd_;
    int events_;
    int revents_;
    int index_; // used by Poller.

    bool eventHandling_;
    bool addedToLoop_;
    bool tied_;
    std::weak_ptr<void> tie_;

    ReadEventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};

} // namespace hvnetpp
