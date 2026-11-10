# hvnetpp 优化清单（面向高并发 RTC SFU）

- 评审时间：2026-11-08，评审基准 commit `e9bbf65`。下文行号以该 commit 为准，代码变动后请按函数名重新定位。
- 优先级：高 / 中 / 低。
- 状态取值：待办 / 进行中 / 部分完成（写明剩余部分）/ 已完成（日期 + 简述实际做法）/ 已失效（说明原因）。

---

## 一、正确性

### C1 任务队列满时回退 mutex 队列，导致跨线程任务乱序
- 优先级：高　状态：已完成（2026-11-09）：换成侵入式无界 Vyukov MPSC（include/hvnetpp/MpscQueue.h），push 永不失败、无回退队列；删除 CircularBuffer 及 /tmp 镜像映射。实测单生产者 200 万任务 FIFO 违例 11→0、4 生产者 479→0
- 位置：`EventLoop::queueInLoop`、`EventLoop::doPendingFunctors`（src/EventLoop.cpp:157-231）；构造函数里的 `PendingQueue(16)`（:56）；`CircularBuffer::createBufferMirror`（src/CircularBuffer.cpp:52-94）
- 问题：
  - 无锁队列只有 1024 个槽（2^16 字节 ÷ 64 字节/节点）。满了以后任务进 `pendingFunctors_`（mutex vector），而 `doPendingFunctors` 总是先排空 MPSC，再执行 vector，跨两个队列的 FIFO 被打破。
  - 回退分支每次调用都会 `RTCLOG(RTC_WARN, ...)`，日志本身要拿全局锁并同步写盘（见 L1），越过载越慢。
  - 镜像映射依赖 `/tmp` 下 mkstemp。容器里 /tmp 只读或不存在时队列失效，之后**每一次** queueInLoop 都走 mutex 并打 WARN。镜像其实没有必要：节点固定 64B，环大小是 64 的整数倍，节点不会跨边界。
- 触发场景：某个 loop 积压了 1024 个任务时，线程 X 调 `ch->write("A")`，A 进了 vector；消费者腾出空位后 X 再调 `ch->write("B")`，B 进了 MPSC，B 先被执行，TCP 字节流变成 B、A。`connectEstablished`/`write`/`close` 之间的顺序同样会被打乱。SFU 里一个线程每个包往另一个 loop 投递一次，几毫秒就能积压到 1024 个。
- 建议：
  - 换成无界的侵入式 MPSC（Vyukov 节点式：push 只做一次 exchange，不会失败，也就不需要回退队列）。
  - 或者先退回 muduo 的 mutex + vector swap。在 T2 解决之前，锁不是瓶颈。
  - 删掉 CircularBuffer 的 /tmp 镜像映射。
  - 与 C2、T2、T3 一起改。

### C2 MPSC 内存序在 ARM 上不安全
- 优先级：高　状态：已完成（2026-11-09）：随 C1 整体替换，旧队列已删除。新队列 push 用 acq_rel exchange + release 链接，pop 用 acquire；已在 aarch64 容器上用 TSan 与多生产者 FIFO 自测验证
- 位置：`MpscQueue::reserve`（include/hvnetpp/MpscQueue.h:43，relaxed 读 head）、`MpscQueue::consume`（:88-95）
- 问题：消费者先 memset 节点，再 relaxed `fetch_add(head)`；生产者用 relaxed 读 head，两边没有 acquire 配对。理论上，消费者迟到的 memset 可能覆盖生产者刚写入的节点，`id` 被清 0 后该槽永远显示"未提交"，`peek` 一直返回 nullptr，队列卡死。另外对 `std::atomic` 做 memset 在形式上是 UB。
- 触发场景：x86（TSO）上不会出现；鲲鹏、Graviton 等 ARM 服务器上可能出现。
- 建议：如果 C1 换掉了这个队列，本条随之失效。若保留，生产者读 head 改为 acquire，`id` 用 store(0) 清零，不用 memset。

### C3 SO_REUSEPORT 默认开启
- 优先级：中　状态：已完成（2026-11-09）：TCP 默认只开 SO_REUSEADDR，`setReusePort(true)` 显式开启；UDP 服务端 SO_REUSEADDR 与 SO_REUSEPORT 默认都不开（对 UDP，SO_REUSEADDR 同样允许重复绑定），`UdpServer` 多 socket 模式自动开启 SO_REUSEPORT，单 socket 可用 `UdpSocketOptions::reusePort`。net_selftest 覆盖端口冲突检测
- 位置：`TcpServer::createsocket`（src/TcpServer.cpp:39）、`UdpServer::createsocket`（src/UdpServer.cpp:40）、`UdpSocket::ensureSocket`（src/UdpSocket.cpp:85）
- 问题与场景：误启动第二个进程时，它也能 bind 成功。内核按四元组把流量分给两个进程，表现为部分会话收不到包，极难排查。
- 建议：默认关闭，提供显式开关（例如 `setReusePort(true)`）。U7 的多 socket 模式内部自行开启。

### C4 可恢复错误直接 abort 整个进程
- 优先级：中　状态：部分完成（2026-11-09）：UDP 路径不再 abort——`UdpSocket::bind` 失败返回 false，UdpServer/UdpClient 建 socket 失败返回 -1。仍待办：`Poller::update` ADD/MOD 失败 abort、`Connector::startConnect` 的 `createNonblockingOrDie`、`Acceptor::listen` 的 `listenOrDie`、accept 遇 ENFILE/ENOBUFS 时的忙循环
- 位置：
  - `Poller::update`（src/Poller.cpp:95-101，ADD/MOD 失败 abort）
  - `UdpSocket::bind` → `sockets::bindOrDie`（src/UdpSocket.cpp:99，端口被占用即 abort）
  - `Connector::startConnect` → `createNonblockingOrDie`（src/Connector.cpp:29，fd 耗尽时重连直接 abort）
  - `Acceptor::listen` → `listenOrDie`
  - `sockets::accept` 遇到 ENFILE/ENOBUFS/ENOMEM 时只打 FATAL 日志（不 abort），`Acceptor::handleRead` 跳出循环；LT 模式下马上再次触发，形成忙循环并刷屏日志
- 建议：
  - 单个 fd 的失败只关闭这一条连接或 socket，并把错误返回给调用方。
  - `UdpSocket::bind` 返回 false。
  - accept 遇到资源类错误时暂停监听一小段时间（定时器恢复），并对日志限频。

### C5 UdpServer/UdpClient 的 sendto 与 stop() 竞争 fd
- 优先级：中　状态：已完成（2026-11-09）：UdpServer/UdpClient 自己持有 fd，`stop()` 只在各 loop 内销毁 UdpSocket（不拥有 fd），fd 保留到析构或 `closesocket()`；`sendto` 以 `started_` 原子标志判定有效期
- 位置：`UdpServer::sendto`/`stop`（src/UdpServer.cpp:78-107）、`UdpClient::send`/`sendto`/`stop`（src/UdpClient.cpp）
- 问题：`fd_` 是普通 int，其他线程读它的同时，loop 线程可能正在关闭这个 fd。fd 编号被新 socket 复用后，媒体包会被发进别的 socket。
- 建议：`fd_` 改为 atomic。stop 时先把 `fd_` 置为 -1，等正在进行的发送结束后（例如用引用计数或读写锁保护关闭时机）再 close。也可以让发送持有 socket 对象的 shared_ptr。

### C6 EventLoop 析构时不清空任务队列
- 优先级：低　状态：已完成（2026-11-09）：`~EventLoop` 先销毁 TimerQueue，再循环排空任务队列（只析构不执行，析构中再投递的也会被排空）。loop_selftest::testDestroyDropsPendingTasks 覆盖
- 位置：`EventLoop::~EventLoop`（src/EventLoop.cpp:72-78）、`MpscQueue::~MpscQueue`
- 问题：loop 退出后才投进来的任务，其 `Functor*` 以及捕获的 shared_ptr（如 `SocketChannelPtr`）永远不会释放。只影响退出阶段。
- 建议：析构时排空队列，只释放、不执行（或执行，二选一并在文档中写明）。

### C8 独立评审（2026-11-09）发现并已修复的问题
- 优先级：中　状态：已完成（2026-11-09）
- 回调里销毁 UdpSocket：回调原本是 UdpSocket 的成员，销毁后正在执行的闭包被释放；libstdc++ 的小闭包内联在 std::function 里，只 move std::function 也不够。改为回调放在独立的 shared_ptr 持有者里，析构时交给延迟任务。net_selftest::testCaptureAliveAfterDestroy 覆盖（去掉修复后 ASan 报 heap-use-after-free）
- `runInLoopAndWait` 的任务因 loop 恰好退出被 `~EventLoop` 丢弃时会永久阻塞：任务内携带通知对象，析构即唤醒，函数返回是否执行；各 stop 路径收到 false 时打 ERROR 并有意泄漏绑定在已退出 loop 上的对象（否则在别的线程注销 Channel 会 abort）。loop_selftest::testRunAndWaitOnExitedLoop 覆盖
- `doPendingFunctors` 摘取阶段无上限（生产者边推边摘）：见 T3，每轮最多 1024 个
- `stopReading()` 后在 loop 线程外析构会 abort（Channel 仍登记在 Poller）：`stopReading()` 改为 disableAll + remove。net_selftest::testDestroyOffThreadAfterStop 覆盖
- `start(false)` 之后在 loop 线程内 `stop()`，排队的初始化任务会在 stop 之后执行：UdpClient/TcpClient 的初始化任务先检查 `started_`
- rtclog：fork 后子进程没有写盘线程——`pthread_atfork` 子进程回调改为同步写；在头文件注释里说明时间戳是写盘时间、崩溃时会丢未写出的日志
- 文档修正：UdpSocket 未 bind 时首次发送会惰性建 fd，须串行；`sendBatch` 拆段发送的项可能部分发出且不计入返回值
- 未修（误用场景或影响很小）：`EventLoopThread::stop()` 与 `startLoop()` 在不同线程并发调用时可能 join 不返回（旧代码就有）；进程退出瞬间与 `shutdown()` 竞争的最后一条异步日志可能丢失

### C7 EventLoopThread::stop() 与 loop 线程退出竞争
- 优先级：高　状态：已完成（2026-11-09）：改为持锁调用 `quit()`
- 位置：`EventLoopThread::stop`（src/EventLoopThread.cpp）
- 问题：原实现在锁内取出 `loop_`，出锁后才 `loop->quit()`。loop 线程可能恰好在这段空隙里退出 `loop()`、析构 EventLoop 并关闭 eventfd，`wakeup()` 随即写到已关闭（甚至被复用）的 fd，还读了已析构对象的成员。TSan 在 tcp_client_test / udp_client_test 退出时报出。muduo 的同名类有同样的问题。
- 修复：线程函数在 `loop()` 返回后要先拿到这把锁把 `loop_` 置空，之后才析构 EventLoop，因此持锁调用 `quit()` 即可保证对象存活。

---

## 二、UDP 数据面（SFU 核心）

### U1 每次可读事件只收 1 个包
- 优先级：高　状态：已完成（2026-11-09）：`recvmmsg` 每批 32 个，单次可读事件最多 256 个后让出
- 位置：`UdpSocket::handleRead`（src/UdpSocket.cpp:115-141）
- 问题：LT 模式下每个包要付出 1 次 `epoll_wait` 加 1 次 `recvfrom`。
- 建议：用 `recvmmsg` 批量收（每批 32–64 个），循环到 EAGAIN 或用完预算为止（防止一个 socket 饿死同一 loop 上的其他 fd）。

### U2 每个包分配一个 Buffer，视频包还要扩容一次
- 优先级：高　状态：已完成（2026-11-09）：回调改为 `onMessage(const UdpPacket&)`（data/len/peer/local/recvTime），收包缓冲每个 loop 线程一份（thread_local），每包零分配零拷贝；recvTime 每批取一次。用户确认直接替换旧签名，示例与 README 已同步
- 位置：`UdpSocket::handleRead` 中的 `Buffer buf; buf.append(...)`（src/UdpSocket.cpp:124-125）；`Buffer::kInitialSize = 1024`（include/hvnetpp/Buffer.h:16）、`Buffer::makeSpace`
- 问题：Buffer 构造时 malloc 1032 字节并清零。视频 RTP 包大多 1100–1200 字节，超过 1024，`append` 时还会再 resize 一次。结果是每个视频包 2 次 malloc/free、2 次 memcpy，外加 1KB 清零。
- 建议：
  - 回调签名改为 `(const InetAddress& peer, const uint8_t* data, size_t len, Timestamp recvTime)`，直接交出收包缓冲区，需要持有数据时由用户从包池拷贝。
  - `recvTime` 每批取一次 steady_clock 即可，TWCC/GCC 带宽估计需要包到达时间。
  - 涉及公共 API 变化，需同步更新 README 和示例。

### U3 EAGAIN 等可预期错误按 ERROR 打日志
- 优先级：中　状态：已完成（2026-11-09）：EAGAIN/EINTR 静默，ECONNREFUSED 与其他错误用 `RTCLOG_EVERY_MS` 限频。另修复：只带 EPOLLERR 的事件（ICMP 错误）原来没人读 SO_ERROR，LT 模式下会一直触发，现在 Channel 的 error 回调会取走它
- 位置：src/UdpSocket.cpp:139
- 问题：空唤醒拿到 EAGAIN/EINTR 时也打 ERROR。connected UDP 收到 ICMP 后，ECONNREFUSED 会持续刷屏。
- 建议：忽略 EAGAIN/EINTR；ECONNREFUSED 等错误限频打印，并可选地回调给用户。

### U4 InetAddress 缺少 operator== 和 std::hash
- 优先级：高　状态：已完成（2026-11-09）：`InetAddress::operator==/!=`、`hash()`（splitmix64）、`std::hash<InetAddress>` 特化、`socklen()`。注意 IPv4 与其 v4-mapped 形式视为不同
- 位置：include/hvnetpp/InetAddress.h
- 问题：SFU 每个包都要按五元组查会话，现在只能用 `toIpPort()` 拼字符串当 key（每包一次分配）。
- 建议：提供不分配内存的 `operator==`、`std::hash<InetAddress>` 特化（按 family 区分 v4/v6，只比较地址和端口）。

### U5 发送端没有批量和分段卸载
- 优先级：中　状态：已完成（2026-11-09）：`sendBatch(const UdpSendItem*, n)` 走 sendmmsg（每次最多 64 条）；`UdpSendItem::gsoSize` 开启 UDP_SEGMENT，内核不支持或遇 EIO 时进程级关闭 GSO 并自动拆包，遇 EINVAL 只对该项拆包；`UdpSocketOptions::gro` 开启 UDP_GRO，收到合并包时库内按段拆开逐个回调。net_selftest 在 gro 开/关两种情况下验证
- 位置：`UdpSocket::sendTo`、`UdpServer::sendto`、`UdpClient::send`
- 问题：SFU 一个包要转发给 N 个订阅者，一帧视频有多个包，目前每个包一次 `sendto`。
- 建议：
  - 提供 `sendmmsg` 批量接口。
  - 发给同一目的地址的一批包，用 UDP GSO（`UDP_SEGMENT`，Linux 4.18+）一次发出。
  - 接收端可选开启 `UDP_GRO`（Linux 5.0+）。

### U6 缺少 SFU 需要的 socket 选项
- 优先级：中　状态：已完成（2026-11-09）：`UdpSocketOptions` 提供 recvBufferSize/sendBufferSize（先试 FORCE，被 rmem_max 截断时打 WARN）、tos、mtuDiscover、recvPktInfo（收包填 `pkt.local`，发送可传 local 指定源 IP，双栈 socket 用 v4-mapped）、gro、reusePort。未提供 IPV6_V6ONLY（需要 bind 前设置，暂按系统默认）
- 需要提供：
  - `SO_RCVBUF`/`SO_SNDBUF`（含 FORCE 版本）：默认约 200KB，流量突发时丢包。
  - `IP_PKTINFO`/`IPV6_RECVPKTINFO`：多网卡机器 bind 0.0.0.0 时，需要用收包时的本地 IP 回包，否则源 IP 不一致会导致 ICE 失败。
  - `IP_TOS`/`IPV6_TCLASS`：DSCP 标记，例如音频标 EF。
  - `IP_MTU_DISCOVER`：DF 位与 PMTU。
  - `IPV6_V6ONLY`：双栈控制。

### U7 UDP 无法利用多核
- 优先级：中　状态：已完成（2026-11-09）：采用方案 1，`UdpServer::setThreadNum(N)` 或 `setEventLoopThreadPool(pool)` 时每个 loop 一个 SO_REUSEPORT socket；`sendto` 在某个 socket 的 loop 线程内调用时用该 socket 发送。方案 2（端口池）可直接把 `UdpSocket` 挂到会话 loop 上。未做 `SO_ATTACH_REUSEPORT_CBPF`
- 问题：一个 UdpServer 只有一个 socket，跑在一个线程上，吞吐上限是单核。
- 建议（二选一，或都支持）：
  1. 单端口模式：每个 worker loop 一个 socket，都 bind 同一端口并开 `SO_REUSEPORT`。内核按四元组哈希，同一客户端固定落在同一线程；需要时用 `SO_ATTACH_REUSEPORT_CBPF` 控制分流。任何线程都能直接 sendto 任意一个 socket，回包源端口不变，发送无需跨线程。
  2. 端口池模式：每个 transport 一个端口，分配到不同的 loop。

---

## 三、线程模型与 API 组合

### T1 Server/Client 自带线程，无法共享外部 loop
- 优先级：高　状态：已完成（2026-11-09）：TcpServer/TcpClient/UdpServer/UdpClient 都有 `(EventLoop* loop, name)` 构造函数；TcpServer/UdpServer 有 `setEventLoopThreadPool(pool)`；`EventLoopThreadPool` 新增 `size()/loop(i)`，baseLoop 可为空。外部 loop 模式下允许在 loop 内 `stop()`。顺带修复 `TcpClient::start(false)` 把局部 promise 的引用交给 loop 线程的悬空问题
- 位置：`TcpServer`（include/hvnetpp/TcpServer.h:71 acceptorThread_）、`UdpServer`（include/hvnetpp/UdpServer.h:48 loopThread_）、`TcpClient`、`UdpClient`
- 问题：SFU 希望同一房间/会话的信令、媒体、定时器都在同一个 loop 线程里运行，从而不需要加锁。现在每个对象各有各的线程，相互之间只能跨线程投递。
- 建议：
  - 构造函数可选传入外部 `EventLoop*` 或 `EventLoopThreadPool*`（参考 libhv 的 `TcpServerEventLoopTmpl(EventLoopPtr loop)`），不传时保持现有行为。
  - `EventLoopThreadPool` 暴露 `loop(i)` / `size()`，让应用自己把会话绑定到 loop。
  - 底层的 `UdpSocket`、`EventLoopThreadPool` 已支持这样用，主要是上层类没有开放。

### T2 跨线程投递成本高
- 优先级：高　状态：部分完成（2026-11-09）：唤醒合并（只有队列由空变非空的生产者写 eventfd，消费者取任务前先清标志）；`queueInLoop/runInLoop` 改为模板，任务对象内联存放可调用体，一次投递一次分配（原来 3 次）。未做：批量投递接口（唤醒合并后收益有限），`SocketChannel::write` 跨线程时仍把数据拷进 std::string（超过 15 字节多一次分配）
- 位置：`SocketChannel::write`（src/SocketChannel.cpp:186-227）、`EventLoop::queueInLoop`（src/EventLoop.cpp:207-231）
- 问题：从别的线程调一次 write，会发生：
  - 拷贝成 std::string（malloc）
  - lambda 捕获 shared_ptr + string 共 48B，超出 std::function 的内联缓冲（malloc）
  - `new Functor`（malloc）
  - 每次都 `write(eventfd)`（1 次系统调用）
- 建议：
  - 唤醒合并：只有从"无待处理任务"变成"有"的那一次才写 eventfd。两边都必须用 exchange，否则会丢唤醒：
    ```cpp
    // 生产者
    push(task);
    if (!wakeupPending_.exchange(true, std::memory_order_acq_rel)) wakeup();
    // 消费者：doPendingFunctors 开头、排空队列之前
    wakeupPending_.exchange(false, std::memory_order_acq_rel);
    drain();
    ```
  - 提供批量投递接口（一次挂一批任务或一批包）。
  - 任务类型改用带内联存储的定长 function（或侵入式任务节点），去掉 `new Functor`。

### T3 自投递的任务会饿死 IO
- 优先级：低　状态：已完成（2026-11-09）：`doPendingFunctors` 先把当前队列整批摘下串成本地链表（复用节点的 mpscNext，不分配）再执行，执行中新投递的留到下一轮；每轮最多 1024 个（多核持续投递时摘取阶段也有界，剩余的下一次 poll 不阻塞）；loop 线程内在非事件阶段投递时只让下一次 poll 不阻塞，不写 eventfd。代价：单生产者满速灌任务时吞吐约低 0–25%（同环境对照：无上限 16–22 Mops/s，1024 上限 15.5–16.3 Mops/s），换来洪泛期间定时器仍准点（loop_selftest::testFloodDoesNotStarveTimers）
- 位置：`EventLoop::doPendingFunctors` 的 while 循环
- 问题：执行过程中新加入的任务会在同一轮被执行，一个不断重新投递自己的任务会让 IO 永远得不到处理。muduo 用 swap 正是为了避免这一点。
- 建议：每轮只执行开始时已在队列中的任务（快照计数或 swap），新任务留到下一轮。

### T4 线程未命名，不支持 CPU 亲和
- 优先级：低　状态：已完成（2026-11-09）：EventLoopThread 启动时 `pthread_setname_np`（取前 15 字节）；`EventLoopThread::setCpuAffinity(cpu)`、`EventLoopThreadPool::setCpuAffinity(cpus)`
- 位置：`EventLoopThread::threadFunc`（src/EventLoopThread.cpp:47）——`name_` 已保存但没调用 `pthread_setname_np`
- 建议：线程启动时设置名字（Linux 上限 15 字节）；`EventLoopThreadPool` 支持传入 CPU 列表做绑核。

---

## 四、定时器

### TM1 TimerQueue 数据结构与 timerfd 开销
- 优先级：中　状态：已完成（2026-11-09）：带索引二叉最小堆 + Timer 对象池（对象在队列生命周期内只回收不释放，过期 TimerId 解引用安全，用 sequence 判定身份）；去掉 timerfd，EventLoop 用堆顶时间算 poll 超时，内核支持时用 epoll_pwait2 纳秒超时，否则毫秒向上取整；同一批到期的定时器被前一个回调取消时不再执行。未做时间轮 / loop 级 tick（堆操作已无分配，应用层可直接 runEvery）。实测增删 380→53 ns/次，1000 个 5ms 周期定时器 CPU 0.094→0.044 核
- 位置：include/hvnetpp/TimerQueue.h:22-25（两个 `std::set`）、`TimerQueue::addTimer`（src/TimerQueue.cpp:78，每个定时器一次 `new Timer`）、`resetTimerfd`
- 问题：
  - keepalive 每次重臂都要 new 一个 Timer、做 2 次红黑树插入、分配一次 std::function；最早到期时间变化时还要调 `timerfd_settime`。
  - `handleRead` 每次 tick 都会分配一个 vector。
  - SFU 每个会话通常有 RTCP、NACK、TWCC、pacer 等多个定时器，会话上万时开销明显。
- 建议：
  - 带索引的最小堆（取消操作 O(log n)，不需要第二个 set），加定时器对象池。
  - keepalive 这类粗粒度超时改用时间轮。
  - 用堆顶时间直接算 `epoll_wait` 的超时，去掉 timerfd。
  - 提供 loop 级固定 tick（例如 5ms，驱动所有会话的 pacer），减少会话级定时器的数量。

### TM2 定时器 API 精度不一致
- 优先级：低　状态：已完成（2026-11-09）：新增 `runAfter/runEvery(std::chrono::duration)` 重载，double 版本改为经 `duration<double>` 转纳秒，不再截断到毫秒
- 位置：`EventLoop::runAfter`/`runEvery`（src/EventLoop.cpp:193-201，截断到毫秒）与 `Timer::restart`（src/Timer.cpp，按微秒计算）
- 建议：API 改用 `std::chrono::duration`（保留 double 版本作兼容），内部统一用微秒或纳秒。

---

## 五、日志

### L1 日志在热路径上拿全局锁并同步写盘
- 优先级：中　状态：已完成（2026-11-09）：级别检查改为原子位图（配置变化时刷新）；默认异步写盘（调用线程格式化后一次 malloc + 无锁入队，后台线程写文件，FATAL 先排空队列再同步写，积压超 100 万条丢弃并计数，atexit 时刷出；`RTC_LOG_SYNC=1` 或 `rtclog_set_async(false)` 改为同步）；新增 `RTCLOG_EVERY_MS` 限频宏、`rtclog_flush()`。rtclog 自身 CMake 为 C++11，改动保持 C++11 兼容
- 位置：`rtclog_is_enabled`（src/thirdparty/rtclog/rtclog.cpp:333）→ `VRegistry::allowed` 中的 `ScopedLock`（src/thirdparty/rtclog/easylogging++.cc:2206）；`rtclog_fmt`（rtclog.cpp:337）
- 问题：只要级别是开启的，检查本身就要拿进程级 mutex，之后在 IO 线程里同步写文件。所有 worker 线程都抢这一把锁，磁盘一卡，所有 loop 一起卡。
- 建议：
  - 级别检查改为读一个 atomic int。
  - 落盘改成异步（后台线程 + 无锁 ring buffer）。
  - 提供限频宏，例如 `RTCLOG_EVERY_N` 或按时间窗口限频。

### L2 日志库通过公共头文件泄漏给使用方
- 优先级：低　状态：待办
- 位置：CMakeLists.txt 中 `src/thirdparty/rtclog` 以 PUBLIC 方式导出
- 问题：使用本库的项目会被带上 easylogging++ 及其全局状态；如果应用自己也用 easylogging，会冲突。
- 建议：库内部定义日志回调接口（级别 + 消息），默认实现可以是 rtclog；公共头文件不再暴露 rtclog。

---

## 六、TCP（SFU 中用于信令 / ICE-TCP / TURN-TCP，优先级较低）

### TCP1 高水位只回调不限流
- 优先级：中　状态：待办
- 位置：src/SocketChannel.cpp:28（默认 64MB）、`SocketChannel::sendInLoop`
- 问题：一个慢订阅者就能占掉 64MB 内存。
- 建议：增加硬上限（超过后断开或丢弃，由策略决定）；默认值调小。

### TCP2 writeBufferSize() 存在数据竞争
- 优先级：低　状态：待办
- 位置：src/SocketChannel.cpp:317-319
- 建议：维护一个 atomic 积压计数，供其他线程读取。

### TCP3 库内全局忽略 SIGPIPE
- 优先级：低　状态：待办
- 位置：src/EventLoop.cpp:34-41（全局静态对象 `IgnoreSigPipe`）
- 建议：socket 写入改用 `send(..., MSG_NOSIGNAL)`（`SocketChannel::handleWrite`/`sendInLoop` 目前用的是 `::write`），去掉全局 signal 设置。

### TCP4 Buffer 只增不减
- 优先级：低　状态：待办
- 问题：每个连接有 3 个 Buffer（input/output/packet），扩容后不会缩回去，大量空闲长连接时内存只涨不降。
- 建议：空闲时（readable 为 0 且容量超过阈值）shrink。

### TCP5 其他小问题
- 优先级：低　状态：待办
- `TcpServer::connections_` 是全局 mutex + `std::map`，每次建立/断开连接都要抢锁。
- 连接 id 是 uint32（`s_channelId`），会回绕，建议改成 uint64。
- `Poller::channels_`（`std::map<int, Channel*>`）只用于 assert/hasChannel，却让每次 add/remove 多一次 O(log n)。

---

## 七、工程

### E1 build.sh 增加 TSan 选项
- 优先级：中　状态：已完成（2026-11-09）：`./script/build.sh --tsan`，产物在 build-tsan/；`--test` 改为跑全部自测。顺带给 build.sh 加上可执行权限（原来 git 中是 100644，`./script/build.sh` 直接执行会失败）
- 说明：C1、C2、C5、TCP2 这类问题，TSan 配合压测基本都能抓到。注意 TSan 不能和 ASan 同时启用。

### E2 UDP pps 基准测试
- 优先级：中　状态：已完成（2026-11-09）：examples/net_bench.cpp（post / udp / timer 三项），数据见文末
- 说明：测单核收、发、echo 的 pps 和 CPU 占用，后续每项性能改动都要有前后对比。

### E3 单元测试
- 优先级：中　状态：已完成（2026-11-09）：examples/loop_selftest.cpp（多生产者 FIFO、丢唤醒、自投递不饿死、析构丢弃任务、定时器顺序/取消/复用/精度、InetAddress 哈希）与 examples/net_selftest.cpp（reuseport 分流与粘性、端口冲突、外部 loop/线程池、sendBatch+GSO/GRO、回调内销毁、TCP 外部 loop）
- 说明：给 MpscQueue（多生产者压测 + 顺序校验）、TimerQueue（取消、重复、回调内取消）补测试。目前只有 `unpack_selftest`。

### E4 logs/ 被提交进 git
- 优先级：低　状态：待办
- 说明：`logs/` 下是示例运行时生成的日志，应加入 `.gitignore` 并 `git rm --cached`。

---

## 2026-11-09 改动前后数据

环境：Apple Silicon Mac 上的 Docker（ubuntu:24.04 arm64，10 vCPU，内核 7.0），Release 构建，net_bench。发送端与接收端在同一台机器上抢 CPU，绝对值只作参考，看相对变化。

| 项目 | 改动前（e9bbf65） | 改动后 |
|---|---|---|
| 跨线程投递，1 生产者 200 万任务 | 2.86 Mops/s，FIFO 违例 11 | 14.8–16.4 Mops/s，违例 0 |
| 跨线程投递，4 生产者各 100 万 | 4.66 Mops/s，FIFO 违例 479 | 14.2 Mops/s，违例 0 |
| 定时器 add+cancel | 380 ns/次 | 55 ns/次 |
| 1000 个 5ms 周期定时器 | 0.094 核 | 0.043 核 |
| UDP 单线程收包，1200B，4 发送线程 | 1.21–1.22 Mpps | 2.35–2.74 Mpps |
| UDP 单线程收包，200B，4 发送线程 | 1.36 Mpps | 2.78–2.79 Mpps |
| UDP 4 线程 reuseport，1200B，6 发送线程 | — | 3.44 Mpps（10 vCPU 已跑满，受限于发送端） |

验证：arm64 GCC 13 下 Debug / ASan+UBSan / UBSan(trap) / TSan 四种构建三套自测全部通过；ASan 与 TSan 构建下示例场景（TCP echo、断线重连、聊天室、UDP 多线程 echo、timer_demo）无报告；amd64 GCC 10（openEuler，旧内核头，走 epoll_wait 回退分支）编译无警告且自测通过。

## 建议顺序（历史）

0–5 已于 2026-11-09 完成（E1/E2/E3、C1/C2/T2/T3、U1–U4、T1/C3/U7/T4、TM1/TM2/L1、U5/U6，以及顺带的 C5/C6/C7/C8）。

剩余待办建议顺序：
1. C4 剩余部分：单个 fd 的 epoll_ctl 失败、fd 耗尽时不 abort；accept 资源类错误暂停监听
2. TCP1：输出缓冲硬上限（慢订阅者）
3. T2 剩余部分：`SocketChannel::write` 跨线程零拷贝
4. L2、TCP2–TCP5、E4
