# hvnetpp

一个 libhv 风格的 C++ Reactor 网络库（仅支持 Linux）。

## 特性

- **多 Reactor 多线程**：accept 独立线程 + `setThreadNum(N)` worker 线程池，one loop per thread；也可以把 Server/Client 挂到外部 loop / 线程池上，让信令、媒体、定时器共享同一个线程
- **TCP 服务端/客户端**：`TcpServer` / `TcpClient`，libhv evpp 风格 API（`onConnection`/`onMessage` 公有回调成员）
- **断线重连**：`reconn_setting_t` 支持固定/线性/指数退避、最大重试次数、连接超时
- **拆包（unpack）**：固定长度 / 分隔符 / 长度字段三种模式，粘包半包自动处理
- **心跳与空闲踢连接**：`setHeartbeat(interval, fn)`、`setKeepaliveTimeout(ms)`
- **UDP（面向 SFU 优化）**：`recvmmsg` 批量收包、零拷贝回调、`SO_REUSEPORT` 多 socket 多核分流、`sendmmsg` 批量发送、UDP GSO/GRO、`IP_PKTINFO` 按目的 IP 回包、收发缓冲 / DSCP / PMTU 等选项；`InetAddress` 可直接作为 `unordered_map` 的 key
- **定时器**：`runAfter` / `runEvery` / `cancel`，带索引最小堆 + 对象池，直接驱动 epoll 超时（不用 timerfd，支持 `epoll_pwait2` 时为纳秒精度）；支持 `std::chrono` 时长
- **底层**：epoll（LT）、无界无锁 MPSC 任务队列（同一线程投递严格 FIFO，唤醒合并）、muduo 风格 Buffer
- **日志**：级别检查无锁，默认异步写盘，提供限频宏 `RTCLOG_EVERY_MS`

## 环境要求

- Linux（依赖 `epoll` / `timerfd` / `eventfd`）
- C++14 编译器（GCC 5+ / Clang）
- CMake 3.10+

## 构建

推荐使用构建脚本（Windows 下请在 WSL 中运行）：

```bash
./script/build.sh                  # Release 构建
./script/build.sh -t Debug --test  # Debug 构建并运行全部自测（unpack/loop/net_selftest）
./script/build.sh -c --ubsan       # 干净重建 UBSan(trap) 版本，无需 libubsan
./script/build.sh --asan           # ASan+UBSan 版本（需系统装有 libasan）
./script/build.sh --tsan --test    # TSan 版本并跑自测（并发相关改动必跑）
./script/build.sh --help           # 全部选项
```

macOS 上可以用 Docker（`ubuntu:24.04`，装 `g++ cmake make`）把仓库挂进容器后运行上面的命令。

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
UdpServer srv("media");
srv.setThreadNum(4);               // 可选：4 线程各一个 socket，SO_REUSEPORT 绑同一端口，内核按四元组分流
UdpSocketOptions opts;
opts.recvBufferSize = 4 << 20;     // 突发流量下少丢包（受 net.core.rmem_max 限制）
opts.recvPktInfo = true;           // 拿到数据报的目的 IP，多网卡时按它回包
srv.setSocketOptions(opts);
srv.createsocket(9999);
// pkt.data 只在回调期间有效；同一对端固定在同一个线程回调
srv.onMessage = [&srv](const UdpPacket& pkt) {
    srv.sendto(pkt.data, pkt.len, pkt.peer, &pkt.local);   // echo，用收包的目的 IP 作源地址
};
srv.start();

UdpClient cli;
cli.createsocket(9999, "127.0.0.1");
cli.setConnectedUdp(true);   // 可选：内核过滤 peer，ICMP 错误可见
cli.onMessage = [](const UdpPacket& pkt) { /* pkt.data, pkt.len, pkt.peer, pkt.recvTime */ };
cli.start();
cli.send(std::string("hello"));
```

批量发送（一次 `sendmmsg`）与 GSO（一帧多个等长包一次交给内核，对端仍收到独立数据报）：

```cpp
UdpSendItem items[2];
items[0].data = rtp1; items[0].len = len1; items[0].peer = &subscriberA;
items[1].data = frame; items[1].len = frameLen; items[1].peer = &subscriberB;
items[1].gsoSize = 1200;               // frame 由若干 1200 字节的包首尾相接而成，末包可更短
int sent = srv.sendBatch(items, 2);    // 返回完整发出的项数；内核/网卡不支持 GSO 时自动拆包发送
```

说明：
- 收包：每次 `recvmmsg` 最多 32 个，单次可读事件最多处理 256 个后让出给同 loop 的其他 fd；收包缓冲每个 loop 线程一份，所有挂在该 loop 上的 socket 共用。
- 发送是数据报语义，不做 EAGAIN 缓冲，失败返回 -1（errno 保留）。
- 端口池模式（每个 transport 一个端口）可以直接用 `UdpSocket`，把它挂到会话所在的 loop 上：`UdpSocket sock(loop); sock.bind(addr); sock.setMessageCallback(cb); sock.startReading();`

### 共享线程（SFU 推荐用法）

```cpp
EventLoopThreadPool pool(nullptr, "sfu");
pool.setThreadNum(8);
pool.setCpuAffinity({0, 1, 2, 3, 4, 5, 6, 7});   // 可选：绑核
pool.start();

UdpServer media("media");
media.setEventLoopThreadPool(&pool);   // 池里每个 loop 一个 reuseport socket
media.createsocket(10000);
media.onMessage = [](const UdpPacket& pkt) {
    // 在 pkt.peer 所属的固定 loop 线程里，按 std::unordered_map<InetAddress, Session*> 查会话，无锁
};
media.start();

TcpServer signaling(pool.loop(0), "signaling");   // accept 跑在池里的某个 loop 上
signaling.setEventLoopThreadPool(&pool);           // 信令连接也分到同一组 loop
signaling.createsocket(8443);
signaling.start();

pool.loop(3)->runEvery(std::chrono::milliseconds(5), [] { /* 该 loop 上所有会话的 pacer */ });
```

## 线程模型

```
默认（自建线程）
TcpServer
  ├─ accept 线程: accept() → round-robin 分发给 worker
  └─ worker 线程 × N: 每线程一个 EventLoop，连接的读写/定时器/回调都在其属主 loop 线程执行
UdpServer
  └─ setThreadNum(N) 个线程（默认 1），每线程一个 SO_REUSEPORT socket，同一对端固定落在同一线程
TcpClient / UdpClient
  └─ 自带 1 个 loop 线程

外部线程（不创建、不拥有线程）
  TcpServer(loop) / TcpClient(loop) / UdpServer(loop) / UdpClient(loop)：挂到外部 loop 上
  TcpServer / UdpServer 的 setEventLoopThreadPool(pool)：连接 / socket 分到外部线程池
```

- 线程安全的接口：`EventLoop::runInLoop/queueInLoop/runInLoopAndWait/runAfter/runEvery/cancel/quit`、`SocketChannel::write/close/shutdown`、`TcpServer::broadcast/foreachChannel/connectionNum`、`TcpClient::send/isConnected/channel`、`UdpServer::sendto/sendBatch`（start 之后）、`UdpClient::send/sendto/sendBatch`
- 跨线程投递：同一线程投递的任务严格 FIFO；稳态开销是一次内存分配 + 一次原子操作，只有目标 loop 的队列由空变非空时才写 eventfd
- 其余配置接口（`setUnpack`、`setSocketOptions` 等）须在 `start()` 前或所属 loop 线程内调用
- `stop()`：使用自建线程时不可在本对象的回调里调用（内部要 join 自己，debug 版有断言）；全部使用外部 loop 时可以在这些 loop 里调用

## 目录结构

- `include/hvnetpp/` — 公共头文件（`hvnetpp.h` 为伞形头）
- `src/` — 实现与内部类（Acceptor / Connector / Poller 等）
- `src/thirdparty/rtclog/` — 日志库
- `examples/` — 示例程序：
  - `tcp_echo_server` — 多线程 echo + keepalive
  - `tcp_chat_server` — 长度字段拆包 + broadcast 聊天室
  - `tcp_client_test` — 重连退避 + 心跳
  - `udp_echo_server` / `udp_client_test` — UDP 往返（多线程 reuseport + pktinfo 回包）
  - `timer_demo` — 定时器用法
  - `unpack_selftest` — 拆包回归测试
  - `loop_selftest` — 任务队列 FIFO / 唤醒 / 定时器 / InetAddress 哈希
  - `net_selftest` — UDP 多 socket 分流、pktinfo、外部 loop、sendBatch、GSO/GRO、TCP 外部 loop
  - `net_bench` — 性能基准

## 注意事项

- **不要在回调里调用 `stop()`（自建线程时）**：`TcpServer`/`TcpClient`/`UdpServer`/`UdpClient` 使用自建线程时，`stop()` 要同步等待并 join 这些线程，在本对象自己的回调里调用会死锁，debug 构建下有断言拦截；需要在回调中触发退出时，请通知外部线程调用 `stop()`。挂在外部 loop 上时没有这个限制。
- **停止顺序**：使用外部 loop / 线程池时，先 `stop()` 挂在上面的 Server/Client，再停 loop / 线程池。顺序反了会打 ERROR，并放弃释放绑定在已退出 loop 上的少量对象（不会卡死，但 loop 对象已析构时属于未定义行为）。
- **任务队列无界**：`queueInLoop` 永不失败，也不做背压；生产速度长期超过 loop 处理能力时内存会持续增长，需要应用层自己限流。每轮最多执行 1024 个任务后回到 IO，保证定时器与 IO 不被饿死。
- **UDP 回调里的数据只在回调期间有效**：`pkt.data` 指向每个 loop 线程共用的收包缓冲，要跨回调保留请自行拷贝。
- **`SO_REUSEPORT` 默认关闭**：避免误起第二个进程也能绑定成功、流量被内核分给两个进程。`UdpServer` 多 socket 模式会自动开启；TCP 需要时 `setReusePort(true)`，UDP 用 `UdpSocketOptions::reusePort`。UDP 服务端也不再设置 `SO_REUSEADDR`（对 UDP 它同样允许重复绑定）。
- **日志默认异步**：调用线程只做格式化和一次无锁入队，后台线程写文件；`FATAL` 同步写出。进程退出时自动刷出，崩溃时可能丢最后几条，调试时可设环境变量 `RTC_LOG_SYNC=1`。高频路径上的错误日志请用 `RTCLOG_EVERY_MS(level, ms, fmt, ...)` 限频。
- **配置接口的调用时机**：`setThreadNum` / `setUnpack` / `setReconnect` / `setConnectTimeout` / `setMaxConnectionNum` 等配置须在 `start()` 之前调用；`SocketChannel` 上的 `setUnpack` / `setKeepaliveTimeout` / `setHeartbeat` 须在其所属 loop 线程内（如 `onConnection` 回调中）调用。线程安全的接口只有明确标注的那些：`write`/`close`/`send`/`broadcast`/`foreachChannel`/`connectionNum` 等。
- **onConnection 一个回调管两件事**：连接建立与断开都走 `onConnection`，用 `ch->isConnected()` 区分，与 libhv 行为一致。
- **拆包回调收到的是完整包**：含包头/分隔符本身；超过 `package_max_length`（默认 2MB）视为协议错误并关闭连接。varint 长度编码暂不支持。
- **UDP 发送不做缓冲**：数据报语义下 `sendto`/`send` 遇 EAGAIN 直接返回 -1（errno 保留），不排队重发，由调用方决定是否重试。
- **新增源文件要手动登记**：CMake 使用显式源文件列表（不用 GLOB），在 `src/` 下新增 `.cpp` 后需同步加进根目录 `CMakeLists.txt` 的 `HVNETPP_SOURCES`。
- **C++ 标准为 C++14**：从旧版的 C++11 升级而来（使用了 lambda init-capture、`std::make_unique` 等）。
- **在 Windows 上开发时 IDE 会误报**：本库仅支持 Linux（依赖 `epoll.h`/`endian.h` 等），Windows 下 clangd/IntelliSense 找不到这些头文件属预期误报，以 Linux（如 WSL）下的实际编译为准；如需消除可为 clangd 配置 WSL 工具链。
- **验证与基准**：`--test` 跑三套自测；并发相关改动请再跑 `--tsan --test`。`net_bench` 用于同一环境下改动前后的性能对比（`post` 跨线程投递、`udp` 收包、`timer` 定时器）。

## 2026-11 SFU 优化的 API 变更（breaking change）

| 旧 | 新 |
|---|---|
| `onMessage = [](const InetAddress& peer, Buffer* buf)`（UdpServer/UdpClient） | `onMessage = [](const UdpPacket& pkt)`，`pkt.data/len/peer/local/recvTime`，数据只在回调期间有效 |
| `UdpSocket::setReadCallback(cb)` | `UdpSocket::setMessageCallback(cb)` |
| `UdpSocket::bind()` 成功后自动开始读；失败 abort | `bind()` 只绑定，失败返回 false；读需显式 `startReading()`（loop 线程） |
| `UdpSocket::sendTo(Buffer*, addr)` | 删除，用 `sendTo(data, len, addr)` |
| `start()` 返回 `void` | 返回 `int`（0 成功 / -1 失败），旧写法照常编译 |
| `UdpSocket::stopReading()` 只关读事件 | 同时从 Poller 摘除，之后可在任意线程析构 |
| TCP/UDP 服务端默认 `SO_REUSEPORT`，UDP 还默认 `SO_REUSEADDR` | 默认都不开（见注意事项） |
| `UdpServer`/`UdpClient` 的 `stop()` 会关闭 fd | fd 保留到析构或 `closesocket()`，使并发的 `sendto` 不会写到被复用的 fd |
| `Timer` 类（`numCreated()` 等）、`CircularBuffer`、`MpscQueue<T>` | `Timer` 改为内部结构体，`CircularBuffer` 删除，`MpscQueue` 改为侵入式无界队列 |
| `EventLoop.h` 间接引入 `TimerQueue.h` / `Channel.h` | 不再间接引入，需要时请直接 include |

新增（不影响旧代码）：外部 loop 构造函数、`setEventLoopThreadPool`、`EventLoopThreadPool::size()/loop(i)/setCpuAffinity()`、`EventLoop::runInLoopAndWait()`、`EventLoop::getEventLoopOfCurrentThread()`、`runAfter/runEvery` 的 `std::chrono` 重载、`UdpSocketOptions`、`sendBatch`、`InetAddress::operator==/hash()/socklen()`、`TcpServer::setReusePort()`、`rtclog_set_async()/rtclog_flush()`、`RTCLOG_EVERY_MS`。

## 与更早的 muduo 风格 API 的差异（breaking change）

旧的 muduo 风格 `TcpServer(loop, addr, name)` + `TcpConnection` + `setXxxCallback()` 接口已移除，替换为 libhv evpp 风格：

| 旧 | 新 |
|---|---|
| `TcpServer(&loop, addr, name)` + 外部 `loop.loop()` | `TcpServer srv; srv.createsocket(port); srv.start();`（内部自管线程） |
| `TcpConnectionPtr` | `SocketChannelPtr` |
| `setConnectionCallback(cb)` | `srv.onConnection = cb;` |
| `conn->send(msg)` | `ch->write(msg)` |
| 无客户端 | `TcpClient`（重连/超时/心跳） |

与 libhv 的差异：`TcpClient::createsocket` 只解析记录地址（每次连接尝试新建 fd 以保证重连干净），返回 0/-1 而非 fd。
