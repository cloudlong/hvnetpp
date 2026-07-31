#pragma once

#include <functional>
#include <memory>

namespace hvnetpp {

class Buffer;
class SocketChannel;

using SocketChannelPtr = std::shared_ptr<SocketChannel>;

using ConnectionCallback = std::function<void(const SocketChannelPtr&)>;
using MessageCallback = std::function<void(const SocketChannelPtr&, Buffer*)>;
using WriteCompleteCallback = std::function<void(const SocketChannelPtr&)>;
using CloseCallback = std::function<void(const SocketChannelPtr&)>;

} // namespace hvnetpp
