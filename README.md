# hvnetpp

一个 libhv 风格的 C++ Reactor 网络库（仅支持 Linux）。

## 特性

- **多 Reactor 多线程**：accept 独立线程 + `setThreadNum(N)` worker 线程池，one loop per thread
- **TCP 服务端/客户端**：`TcpServer` / `TcpClient`，libhv evpp 风格 API（`onConnection`/`onMessage` 公有回调成员）
- **断线重连**：`reconn_setting_t` 支持固定/线性/指数退避、最大重试次数、连接超时
- **拆包（unpack）**：固定长度 / 分隔符 / 长度字段三种模式，粘包半包自动处理
- **心跳与空闲踢连接**：`setHeartbeat(interval, fn)`、`setKeepaliveTimeout(ms)`
- **UDP**：`UdpServer` / `UdpClient`（可选 connected-UDP 模式）
- **定时器**：`runAfter` / `runEvery` / `cancel`，基于 timerfd + steady_clock
- **底层**：epoll（LT）、eventfd 唤醒、无锁 MPSC 任务队列、muduo 风格 Buffer

## 环境要求

- Linux（依赖 `epoll` / `timerfd` / `eventfd`）
- C++14 编译器（GCC 5+ / Clang）
- CMake 3.10+

## 构建

推荐使用构建脚本（Windows 下请在 WSL 中运行）：

```bash
./script/build.sh                  # Release 构建
./script/build.sh -t Debug --test  # Debug 构建并运行拆包回归测试
./script/build.sh -c --ubsan       # 干净重建 UBSan(trap) 版本，无需 libubsan
./script/build.sh --asan           # ASan+UBSan 版本（需系统装有 libasan）
./script/build.sh --help           # 全部选项
```

或直接用 CMake：

```bash
cmake -S . -B build
cmake --build build -j
# 示例在 build/examples/ 下；关闭示例: -DHVNETPP_BUILD_EXAMPLES=OFF
```

## 快速上手

### TCP 服务端

```cpp
#include "hvnetpp/hvnetpp.h"
using namespace hvnetpp;

int main() {
    TcpServer srv("echo");
    srv.createsocket(1234);          // 建 socket + bind，失败返回 -1
    srv.setThreadNum(4);             // 4 个 worker 线程；0 = 所有 IO 在 accept 线程
    srv.setKeepaliveTimeout(60000);  // 60 秒无数据踢掉连接（可选）

    srv.onConnection = [](const SocketChannelPtr& ch) {
        // 建立与断开都走这里，用 isConnected() 区分
        printf("%s %s\n", ch->peeraddr().c_str(), ch->isConnected() ? "connected" : "disconnected");
    };
    srv.onMessage = [](const SocketChannelPtr& ch, Buffer* buf) {
        ch->write(buf);              // echo；write/close 线程安全
    };

    srv.start();                     // 返回后即开始服务
    // ... 阻塞等待退出信号 ...
    srv.stop();                      // 关闭全部连接并 join 线程（勿在回调中调用）
}
```

### TCP 客户端（重连 + 心跳）

```cpp
TcpClient cli("demo");
cli.createsocket(1234, "127.0.0.1");   // 支持域名
cli.setConnectTimeout(3000);

reconn_setting_t reconn;               // 默认 1s 起指数退避、上限 60s、无限重试
reconn.max_retry_cnt = 10;
cli.setReconnect(&reconn);

cli.setHeartbeat(5000, [&cli] { cli.send(std::string("ping")); });

cli.onConnection = [](const SocketChannelPtr& ch) { /* ... */ };
cli.onMessage    = [](const SocketChannelPtr& ch, Buffer* buf) { /* ... */ };
cli.onConnectFailed = [] { /* 每次尝试失败回调 */ };

cli.start();
cli.send(std::string("hello"));
// ...
cli.stop();
```

### 拆包（unpack）

设置后 `onMessage` 每次收到的都是一个完整包（含包头/分隔符）：

```cpp
unpack_setting_t s;

// 1) 固定长度
s.mode = UNPACK_BY_FIXED_LENGTH;
s.fixed_length = 16;

// 2) 分隔符（如 \r\n）
s.mode = UNPACK_BY_DELIMITER;
s.delimiter[0] = '\r'; s.delimiter[1] = '\n';
s.delimiter_bytes = 2;

// 3) 长度字段: [4 字节 BE 包体长度][包体]
//    packet_len = body_offset + 长度字段值 + length_adjustment
s.mode = UNPACK_BY_LENGTH_FIELD;
s.body_offset = 4;            // 包头长度
s.length_field_offset = 0;
s.length_field_bytes = 4;     // 1/2/4/8
s.length_field_coding = ENCODE_BY_BIG_ENDIAN;

srv.setUnpack(&s);   // TcpServer / TcpClient / SocketChannel 均可设置
```

超过 `package_max_length`（默认 2MB）视为协议错误并关闭连接。

### UDP

```cpp
UdpServer srv;
srv.createsocket(9999);
srv.onMessage = [&srv](const InetAddress& peer, Buffer* buf) {
    srv.sendto(buf->retrieveAllAsString(), peer);   // echo
};
srv.start();

UdpClient cli;
cli.createsocket(9999, "127.0.0.1");
cli.setConnectedUdp(true);   // 可选：内核过滤 peer，ICMP 错误可见
cli.onMessage = [](const InetAddress& peer, Buffer* buf) { /* ... */ };
cli.start();
cli.send(std::string("hello"));
```

注：数据报语义下发送不做 EAGAIN 缓冲，失败返回 -1（errno 保留）。

## 线程模型

```
TcpServer
  ├─ acceptor 线程: accept() → round-robin 分发给 worker
  └─ worker 线程 × N: 每线程一个 EventLoop，连接的读写/定时器/回调都在其属主 loop 线程执行
TcpClient / UdpServer / UdpClient
  └─ 自带 1 个 loop 线程
```

- `SocketChannel::write() / close()`、`TcpServer::broadcast()/foreachChannel()/connectionNum()`、`TcpClient::send()` 线程安全，可跨线程调用
- 其余配置接口（`setUnpack` 等）须在 `start()` 前或所属 loop 线程内调用
- `stop()` 不可在本对象自己的回调中调用（内部同步等待会死锁，debug 版有断言）

## 目录结构

- `include/hvnetpp/` — 公共头文件（`hvnetpp.h` 为伞形头）
- `src/` — 实现与内部类（Acceptor / Connector / Poller 等）
- `src/thirdparty/rtclog/` — 日志库
- `examples/` — 示例程序：
  - `tcp_echo_server` — 多线程 echo + keepalive
  - `tcp_chat_server` — 长度字段拆包 + broadcast 聊天室
  - `tcp_client_test` — 重连退避 + 心跳
  - `udp_echo_server` / `udp_client_test` — UDP 往返
  - `timer_demo` — 定时器用法
  - `unpack_selftest` — 拆包回归测试

## 注意事项

- **不要在回调里调用 `stop()`**：`TcpServer`/`TcpClient`/`UdpServer`/`UdpClient` 的 `stop()` 内部会同步等待自有 loop 线程执行完关闭流程，在本对象自己的回调（即其 loop 线程）里调用会死锁，debug 构建下有断言拦截。需要在回调中触发退出时，请通知外部线程（如信号量/条件变量）由外部调用 `stop()`。
- **配置接口的调用时机**：`setThreadNum` / `setUnpack` / `setReconnect` / `setConnectTimeout` / `setMaxConnectionNum` 等配置须在 `start()` 之前调用；`SocketChannel` 上的 `setUnpack` / `setKeepaliveTimeout` / `setHeartbeat` 须在其所属 loop 线程内（如 `onConnection` 回调中）调用。线程安全的接口只有明确标注的那些：`write`/`close`/`send`/`broadcast`/`foreachChannel`/`connectionNum` 等。
- **onConnection 一个回调管两件事**：连接建立与断开都走 `onConnection`，用 `ch->isConnected()` 区分，与 libhv 行为一致。
- **拆包回调收到的是完整包**：含包头/分隔符本身；超过 `package_max_length`（默认 2MB）视为协议错误并关闭连接。varint 长度编码暂不支持。
- **UDP 发送不做缓冲**：数据报语义下 `sendto`/`send` 遇 EAGAIN 直接返回 -1（errno 保留），不排队重发，由调用方决定是否重试。
- **新增源文件要手动登记**：CMake 使用显式源文件列表（不用 GLOB），在 `src/` 下新增 `.cpp` 后需同步加进根目录 `CMakeLists.txt` 的 `HVNETPP_SOURCES`。
- **C++ 标准为 C++14**：从旧版的 C++11 升级而来（使用了 lambda init-capture、`std::make_unique` 等）。
- **在 Windows 上开发时 IDE 会误报**：本库仅支持 Linux（依赖 `epoll.h`/`endian.h` 等），Windows 下 clangd/IntelliSense 找不到这些头文件属预期误报，以 Linux（如 WSL）下的实际编译为准；如需消除可为 clangd 配置 WSL 工具链。
- **建议的额外验证**：本库已在 UBSan（trap 模式）+ `_GLIBCXX_ASSERTIONS` 下通过 echo/重连/聊天室压测；若你的环境装有 libasan，建议再以 `-fsanitize=address` 构建复跑一遍示例场景。

## 与旧版 API 的差异（breaking change）

旧的 muduo 风格 `TcpServer(loop, addr, name)` + `TcpConnection` + `setXxxCallback()` 接口已移除，替换为 libhv evpp 风格：

| 旧 | 新 |
|---|---|
| `TcpServer(&loop, addr, name)` + 外部 `loop.loop()` | `TcpServer srv; srv.createsocket(port); srv.start();`（内部自管线程） |
| `TcpConnectionPtr` | `SocketChannelPtr` |
| `setConnectionCallback(cb)` | `srv.onConnection = cb;` |
| `conn->send(msg)` | `ch->write(msg)` |
| 无客户端 | `TcpClient`（重连/超时/心跳） |

与 libhv 的差异：`TcpClient::createsocket` 只解析记录地址（每次连接尝试新建 fd 以保证重连干净），返回 0/-1 而非 fd。
