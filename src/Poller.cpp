#include "hvnetpp/Poller.h"
#include "hvnetpp/Channel.h"
#include "rtclog.h"
#include <sys/epoll.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <assert.h>
#include <cerrno>

namespace hvnetpp {

Poller::Poller(EventLoop* loop)
    : loop_(loop),
      epollfd_(::epoll_create1(EPOLL_CLOEXEC)),
      events_(kInitEventListSize) {
    if (epollfd_ < 0) {
        RTCLOG(RTC_FATAL, "Poller::Poller error: %s", strerror(errno));
        std::abort();
    }
}

Poller::~Poller() {
    ::close(epollfd_);
}

void Poller::poll(int timeoutMs, ChannelList* activeChannels) {
    int numEvents = ::epoll_wait(epollfd_, &*events_.begin(), static_cast<int>(events_.size()), timeoutMs);
    int savedErrno = errno;
    if (numEvents > 0) {
        fillActiveChannels(numEvents, activeChannels);
        if (static_cast<size_t>(numEvents) == events_.size()) {
            events_.resize(events_.size() * 2);
        }
    } else if (numEvents == 0) {
        // nothing happened
    } else {
        if (savedErrno != EINTR) {
            errno = savedErrno;
            RTCLOG(RTC_ERROR, "Poller::poll() error: %s", strerror(savedErrno));
        }
    }
}

void Poller::fillActiveChannels(int numEvents, ChannelList* activeChannels) const {
    for (int i = 0; i < numEvents; ++i) {
        Channel* channel = static_cast<Channel*>(events_[i].data.ptr);
        channel->set_revents(events_[i].events);
        activeChannels->push_back(channel);
    }
}

void Poller::updateChannel(Channel* channel) {
    const int index = channel->index();
    if (index == -1 || index == 2) {
        if (channel->isNoneEvent()) {
            return;
        }
        update(EPOLL_CTL_ADD, channel);
        channel->set_index(1);
        channels_[channel->fd()] = channel;
    } else {
        // update existing one
        if (channel->isNoneEvent()) {
            update(EPOLL_CTL_DEL, channel);
            channel->set_index(2); // deleted
        } else {
            update(EPOLL_CTL_MOD, channel);
        }
    }
}

void Poller::removeChannel(Channel* channel) {
    int fd = channel->fd();
    assert(channels_.find(fd) != channels_.end());
    assert(channels_[fd] == channel);
    assert(channel->isNoneEvent());
    int index = channel->index();
    assert(index == 1 || index == 2);
    size_t n = channels_.erase(fd);
    assert(n == 1);

    if (index == 1) {
        update(EPOLL_CTL_DEL, channel);
    }
    channel->set_index(-1);
}

void Poller::update(int operation, Channel* channel) {
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = channel->events();
    event.data.ptr = channel;
    int fd = channel->fd();
    if (::epoll_ctl(epollfd_, operation, fd, &event) < 0) {
        if (operation == EPOLL_CTL_DEL) {
            RTCLOG(RTC_ERROR, "epoll_ctl op=%d fd=%d error: %s", operation, fd, strerror(errno));
        } else {
            RTCLOG(RTC_FATAL, "epoll_ctl op=%d fd=%d error: %s", operation, fd, strerror(errno));
            std::abort();
        }
    }
}

bool Poller::hasChannel(Channel* channel) const {
    auto it = channels_.find(channel->fd());
    return it != channels_.end() && it->second == channel;
}

} // namespace hvnetpp
