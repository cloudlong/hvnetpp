#pragma once

#include <cstdint>
#include <map>
#include <vector>

struct epoll_event;

namespace hvnetpp {

class Channel;
class EventLoop;

class Poller {
public:
    using ChannelList = std::vector<Channel*>;

    Poller(EventLoop* loop);
    ~Poller();

    // timeoutNs: 0 = 不阻塞；精度为纳秒（内核支持 epoll_pwait2 时），否则向上取整到毫秒
    void poll(int64_t timeoutNs, ChannelList* activeChannels);
    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);

    bool hasChannel(Channel* channel) const;

private:
    static const int kInitEventListSize = 16;

    void fillActiveChannels(int numEvents, ChannelList* activeChannels) const;
    void update(int operation, Channel* channel);

    using EventList = std::vector<struct epoll_event>;

    EventLoop* loop_;
    int epollfd_;
    EventList events_;
    std::map<int, Channel*> channels_;
};

} // namespace hvnetpp
