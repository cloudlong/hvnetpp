# hvnetpp

libhv 风格的 C++14 Reactor 网络库，仅支持 Linux（epoll / eventfd）。目标是比 libhv、libuv 更简洁好用，**后续将作为高并发 WebRTC SFU 的网络底座**。

评估任何改动时，优先考虑 SFU 的热路径：
- UDP 收发吞吐（pps）
- 每包的内存分配与系统调用次数
- 跨线程投递开销
- 多核扩展
- 定时器规模

TCP 主要用于信令、ICE-TCP、TURN-TCP，优先级次之。

## 构建与验证

- 只能在 Linux 下编译（WSL、Docker 或远程机器）。macOS/Windows 本机缺 `sys/epoll.h`、`endian.h` 等头文件，IDE 报错属预期。macOS 上的 Docker 用法见 `hvnetpp-optimize` skill。
- 常用命令：
  - `./script/build.sh -t Debug --test`：Debug 构建，并运行全部自测（`unpack_selftest`、`loop_selftest`、`net_selftest`）
  - `./script/build.sh -c --asan`、`--ubsan`、`--tsan`：sanitizer 构建，产物在 `build-asan/`、`build-ubsan/`、`build-tsan/`；并发相关改动必须跑 `--tsan --test`
  - `./script/run_examples.sh <build_dir>`：跑一遍示例场景（TCP echo / 重连 / 聊天室 / UDP / 定时器）并检查 sanitizer 报告
  - `build/examples/net_bench post|udp|timer`：性能基准，只用于同一环境下的前后对比
- 时序相关的自测断言用轮询等待，不要依赖固定 sleep（TSan 下会慢数倍）。

## 目录

- `include/hvnetpp/`：公共头文件，`hvnetpp.h` 为伞形头
- `src/`：实现。`Acceptor`、`Connector`、`UdpIo`（UDP fd 级收发与选项）是内部类/函数，头文件也在 `src/`
- `src/thirdparty/rtclog/`：日志（easylogging++ 的封装），宏为 `RTCLOG(level, fmt, ...)`、限频版 `RTCLOG_EVERY_MS(level, ms, fmt, ...)`
- `examples/`：示例程序、自测（`*_selftest`）、基准（`net_bench`）
- `script/`：`build.sh`、`run_examples.sh`
- `logs/`：示例运行时生成的日志（不应提交，待加入 .gitignore，见 E4）

## 架构要点

- one loop per thread：`EventLoop` 由以下部分组成：
  - `Poller`：epoll LT；超时为纳秒，内核支持时用 `epoll_pwait2`，否则 `epoll_wait` 向上取整到毫秒
  - `TimerQueue`：带索引最小堆 + Timer 对象池，不用 timerfd；loop 用堆顶时间算 poll 超时，每轮 IO 之后执行到期定时器
  - 任务队列：侵入式无界 Vyukov MPSC（`MpscQueue.h`），同一线程投递严格 FIFO；跨线程投递只有队列由空变非空时才写 eventfd；每轮只执行开始时已入队的任务
- `Channel`：fd 与回调的绑定。`tie()` 绑定 owner 的 weak_ptr，处理事件期间保活 owner。
- `SocketChannel`：一条 TCP 连接，由 shared_ptr 管理，对应 libhv 的 SocketChannel。
- `UdpSocket`：可挂在任意 loop 上的 UDP socket。`recvmmsg` 批量收包，收包缓冲每个 loop 线程一份（thread_local），回调 `onMessage(const UdpPacket&)` 零拷贝；`sendBatch` 走 sendmmsg，支持 GSO；回调里可安全销毁自身。
- `UdpServer`：每个 loop 一个 socket，多个时用 `SO_REUSEPORT` 绑同一端口；fd 由 UdpServer 持有到析构，`stop()` 只销毁各 loop 里的 UdpSocket。
- `TcpServer`：accept loop + worker loops，round-robin 分配连接。
- 线程来源：TcpServer/TcpClient/UdpServer/UdpClient 默认自建线程；也可以用 `(EventLoop* loop, name)` 构造挂到外部 loop，TcpServer/UdpServer 用 `setEventLoopThreadPool(pool)` 共享外部线程池。SFU 推荐所有组件共享一个 `EventLoopThreadPool`，同一会话的信令、媒体、定时器在同一线程。

## 约定与陷阱

- 新增 `src/*.cpp` 必须手动加入根 `CMakeLists.txt` 的 `HVNETPP_SOURCES`（不用 GLOB）；新增示例/自测要加入 `examples/CMakeLists.txt` 的 `HVNETPP_EXAMPLES`。
- 库代码 C++14；`src/thirdparty/rtclog/` 自带的 CMake 是 C++11，改它时保持 C++11 兼容。代码注释、日志和文档用中文。
- 线程安全的接口只有以下这些，其余配置须在 `start()` 前或在所属 loop 线程内调用：
  - `EventLoop::runInLoop/queueInLoop/runInLoopAndWait/runAt/runAfter/runEvery/cancel/quit`
  - `SocketChannel::write/close/shutdown`
  - `TcpServer::broadcast/foreachChannel/connectionNum`
  - `TcpClient::send/isConnected/channel`
  - `UdpServer::sendto/sendBatch`（start 之后）、`UdpClient::send/sendto/sendBatch`
- `stop()`：自建线程时不能在本对象的回调里调用（要 join 自己，debug 构建下有断言）；全部使用外部 loop 时可以在这些 loop 里调用。`runInLoopAndWait` 不要在两个互相等待的 loop 线程之间使用。
- `Channel` 不能在它自己的事件回调里被同步析构：先 `disableAll()` + `remove()`，再把 shared_ptr 交给 `queueInLoop` 延迟释放。参考 `Connector::removeAndResetChannel` 和 `UdpSocket::~UdpSocket`。同理，可能在某对象回调链里被调用的 stop 路径要延迟析构该对象（参考 `TcpClient::stop` 对 Connector 的处理）。
- 用户回调可能在执行中销毁其所属对象时（如 UdpSocket），回调要放在独立的堆持有者里，析构时交给 `queueInLoop` 延迟释放；libstdc++ 的小闭包内联在 std::function 里，只延迟 Channel 或只 move std::function 都不够。
- 停止顺序：先 stop 挂在外部 loop 上的 Server/Client，再停 loop/线程池。`runInLoopAndWait` 返回 false 表示 loop 已退出、任务没执行，stop 路径据此放弃绑定在该 loop 上的对象而不是 abort。
- 定时器回调一律捕获 weak_ptr，不要捕获裸 this 或 shared_ptr（参考 `SocketChannel::armKeepaliveTimer`）。
- 跨线程停 loop 时，`quit()` 必须在 EventLoop 保证存活的前提下调用（参考 `EventLoopThread::stop` 持锁调用）。
- `SO_REUSEPORT` 默认关闭（防止误起两个进程都绑定成功）；UDP 服务端也不设 `SO_REUSEADDR`。
- 热路径（每包、每次投递）上不要加同步日志；错误日志用 `RTCLOG_EVERY_MS` 限频。日志默认异步写盘，`RTC_LOG_SYNC=1` 可改为同步。
- 修改公共 API 时，同步更新 README 的快速上手、线程模型、注意事项、API 变更表等章节。

## 优化清单

2026-11-08 做过一次面向 SFU 的全量评审，2026-11-09 完成了其中大部分（任务队列、UDP 收发、外部 loop/多核、定时器、日志、测试与基准）。全部条目（含位置、做法、改动前后数据、剩余待办）记录在 [.claude/skills/hvnetpp-optimize/backlog.md](.claude/skills/hvnetpp-optimize/backlog.md)。处理清单条目或评审改动时，使用 `hvnetpp-optimize` skill。

剩余最重要的几项：
1. C4 剩余部分：`Poller::update` 的 epoll_ctl 失败、`Connector` 建 socket 失败仍会 abort 进程；accept 遇 ENFILE 会忙循环
2. TCP1：TCP 输出缓冲只有高水位回调，没有硬上限
3. T2 剩余部分：`SocketChannel::write` 跨线程时仍拷贝一次数据
