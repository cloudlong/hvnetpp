# hvnetpp

A lightweight C++ network framework based on the Reactor pattern.

## Introduction

hvnetpp is a high-performance, non-blocking network library for C++. It uses `epoll` for event notification and provides a simple API for building TCP servers and UDP applications. It is designed to be easy to understand and use, making it suitable for learning network programming or building small to medium-sized network applications on Linux.

## Features

- **Non-blocking I/O**: Based on the Reactor pattern using `epoll` (Linux only).
- **TCP Support**: Easy-to-use `TcpServer` and `TcpConnection` classes for handling TCP connections.
- **UDP Support**: explicit `UdpSocket` lifecycle for bind/send/receive.
- **Address Utilities**: `InetAddress` as a concrete endpoint value type plus a separate `Resolver` for DNS.
- **Timers**: Efficient timer management via `TimerQueue`.
- **Callbacks**: Modern C++11 callbacks (`std::function`) for connection establishment, message reception, and write completion.
- **Logging**: Integrated logging via `rtclog`.

## Requirements

- **Operating System**: Linux (requires `sys/epoll.h`).
- **Compiler**: C++11 compliant compiler (e.g., GCC, Clang).
- **Build System**: CMake 3.10 or higher.

## Build Instructions

1.  Clone the repository:
    ```bash
    git clone https://github.com/walkerlinrtc/hvnetpp.git
    cd hvnetpp
    ```

2.  Create a build directory:
    ```bash
    mkdir build
    cd build
    ```

3.  Run CMake and make:
    ```bash
    cmake ..
    make
    ```

4.  Run the test server (if built):
    ```bash
    ./test_build
    ```

## Usage Example

Below is a simple example of a TCP Server that logs connections and messages:

```cpp
#include "hvnetpp/EventLoop.h"
#include "hvnetpp/TcpServer.h"
#include "rtclog.h"
#include <iostream>

int main() {
    // Initialize logger
    rtclog_init("TestServer");
    rtclog_set_level(RTC_INFO);

    hvnetpp::EventLoop loop;
    hvnetpp::InetAddress addr(9999); // Listen on port 9999

    hvnetpp::TcpServer server(&loop, addr, "TestServer");

    // Set connection callback
    server.setConnectionCallback([](const hvnetpp::TcpConnectionPtr& conn) {
        if (conn->connected()) {
            RTCLOG(RTC_INFO, "Client connected: %s", conn->peerAddress().toIpPort().c_str());
        } else {
            RTCLOG(RTC_INFO, "Client disconnected: %s", conn->peerAddress().toIpPort().c_str());
        }
    });

    // Set message callback
    server.setMessageCallback([](const hvnetpp::TcpConnectionPtr& conn, hvnetpp::Buffer* buf) {
        std::string msg = buf->retrieveAllAsString();
        RTCLOG(RTC_INFO, "Received %d bytes: %s", msg.size(), msg.c_str());
        conn->send(msg); // Echo back
    });

    server.start();
    loop.loop(); // Start the event loop

    return 0;
}
```

## Address And DNS

`InetAddress` is the public endpoint value type. It stores one concrete `sockaddr` and is responsible for parse/format helpers such as `toIp()` and `toIpPort()`.

Hostname resolution lives in `Resolver` instead of `InetAddress`:

```cpp
#include "hvnetpp/Resolver.h"
#include <sys/socket.h>

hvnetpp::InetAddress addr;
int resolveError = 0;
if (hvnetpp::Resolver::resolve("localhost", &addr, 8080, AF_UNSPEC, SOCK_STREAM, &resolveError)) {
    // addr now contains one resolved endpoint
} else {
    RTCLOG(RTC_ERROR, "resolve failed: %s", hvnetpp::Resolver::errorString(resolveError));
}
```

`SocketsOps` is now an internal implementation detail under `src/` and is no longer part of the public include surface.

For raw IP literals, prefer `InetAddress::tryParse(...)` when the input comes from users or config files.

## UDP Lifecycle

`UdpSocket` now uses a more explicit lifecycle:

```cpp
hvnetpp::UdpSocket udp(&loop, "echo-udp");
udp.setReadCallback([](const hvnetpp::InetAddress& peer, const void* data, size_t len) {
    // `data` is owned by the socket and is only valid during the callback
});

if (udp.bindAndStart(hvnetpp::InetAddress::any(9000))) {
    // receiving is now active
}
```

`bind()` no longer starts read interest implicitly, `startReceive()` now reports failure instead of silently doing nothing on an unbound socket, and `sendTo(const Buffer&)` does not mutate the caller's buffer. For sender-only sockets, `sendTo(...)` still lazily opens the underlying UDP fd.

## TCP Sending And Start State

`TcpConnection::send(const std::string&)`, `send(const void*, size_t)`, and `send(const Buffer&)` copy bytes into the send path and do not mutate caller-owned memory. The legacy `send(Buffer*)` overload is still available as a transfer-style compatibility API and clears the readable bytes from the buffer.

`TcpServer::start()` now returns a state value:

- `kListening`: the socket is already listening now
- `kStarting`: the start request was queued to the loop thread
- `kIdle`: an immediate start attempt failed and the server stayed idle

## Timer API

The timer API accepts both the legacy `double` seconds form and `std::chrono` durations. New code can use the chrono overloads directly:

```cpp
using namespace std::chrono;

loop.runAfter(2500ms, []() {
    RTCLOG(RTC_INFO, "fires once after 2.5 seconds");
});

hvnetpp::TimerId id = loop.runEvery(1s, []() {
    RTCLOG(RTC_INFO, "fires every second");
});

if (id) {
    loop.cancel(id);
}
```

## Directory Structure

- `include/hvnetpp/`: Public header files.
- `src/`: Source code implementation.
  - `thirdparty/`: Third-party libraries (e.g., rtclog).
- `test_build.cpp`: Example usage file.
