#pragma once

#include "hvnetpp/Buffer.h"
#include <atomic>
#include <memory>
#include <functional>
#include <sys/types.h>
#include <vector>
#include "hvnetpp/InetAddress.h"

namespace hvnetpp {

class EventLoop;
class Channel;

class UdpSocket {
public:
    enum class State {
        kClosed,
        kOpened,
        kBound,
        kReceiving,
    };

    // Runs on the owning loop thread. The memory is owned by the socket and is only valid during the callback.
    using ReadCallback = std::function<void(const InetAddress& peerAddr, const void* data, size_t len)>;
    
    UdpSocket(EventLoop* loop, const std::string& name);
    ~UdpSocket();

    // Optional pre-open step. Use this to pin the family and surface socket-creation errors early.
    bool open(sa_family_t family);

    // Safe to call from any thread. Lazily opens the socket if needed.
    // Binding does not implicitly enable receiving; call startReceive() when ready.
    bool bind(const InetAddress& addr);
    // Convenience helper for the common server case.
    bool bindAndStart(const InetAddress& addr);
    // Safe to call from any thread.
    // Returns false if the socket has not been bound yet.
    bool startReceive();
    // Safe to call from any thread.
    void stopReceive();
    void setReadCallback(ReadCallback cb) { readCallback_ = std::move(cb); }
    
    // Safe to call from any thread. Lazily opens the socket if needed.
    ssize_t sendTo(const void* data, size_t len, const InetAddress& destAddr);
    // Does not mutate `buf`.
    ssize_t sendTo(const Buffer& buf, const InetAddress& destAddr);

    State state() const { return state_.load(std::memory_order_acquire); }
    bool isOpen() const { return state() != State::kClosed; }
    bool isBound() const { return state() == State::kBound || state() == State::kReceiving; }
    bool isReceiving() const { return state() == State::kReceiving; }
    int fd() const { return sockfd_; }

private:
    bool ensureSocket(sa_family_t family);
    void handleRead();

    EventLoop* loop_;
    const std::string name_;
    sa_family_t family_;
    int sockfd_;
    std::atomic<State> state_;
    std::shared_ptr<Channel> channel_;
    std::shared_ptr<bool> callbackToken_;
    ReadCallback readCallback_;
    std::vector<char> readBuf_; // UDP packet buffer
};

} // namespace hvnetpp
