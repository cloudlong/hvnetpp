---
name: hvnetpp-optimize
description: 处理 hvnetpp 优化清单（backlog.md）中的条目，或按高并发 RTC SFU 的标准评审、实现网络库改动（UDP 收发路径、跨线程任务队列、定时器、日志、多核扩展）。当用户说"按清单优化""修 C1/U2 这类编号""改 MPSC/UDP/定时器""评审这次改动的性能"时使用。
---

# hvnetpp 优化流程

hvnetpp 将作为高并发 SFU 的网络底座。本 skill 有两个用途：逐项落实 [backlog.md](backlog.md) 中的问题；按 SFU 热路径标准检查新改动。

## 处理清单条目

1. **选条目**：用户指定了编号就做那一项；没指定时，按 backlog.md 末尾"建议顺序"取第一个状态为"待办"的条目。一次只做一个条目，或一组明确相关的条目（例如 C1+C2+T2+T3），不要顺手改别的地方。
2. **复核问题是否仍然存在**：
   - backlog 的行号以 commit `e9bbf65` 为准，代码可能已经变了，按条目里写的函数名重新定位。
   - 动手前读完整条调用链。例如改任务队列，要同时读 `EventLoop::queueInLoop`、`doPendingFunctors`、`MpscQueue`、`CircularBuffer`，并检查所有 `queueInLoop`/`runInLoop` 调用方对执行顺序的依赖（`TcpServer::stop` 的栅栏、`Connector::removeAndResetChannel` 的延迟析构等）。
   - 如果问题已经不存在，把状态改为"已失效"并写明原因。
3. **定方案**：
   - 涉及公共 API 变化时（例如 UDP 回调签名、Server 构造函数），先向用户给出新旧 API 对比，确认后再改。
   - 保持 libhv 风格：公有回调成员 `onXxx`，以及 `createsocket` / `start` / `stop`。
   - 保持 C++14；未经用户同意不引入新的第三方依赖。
   - 热路径上不新增：每包 malloc、每包系统调用、全局锁、同步日志。
4. **实现**：
   - 新增 `src/*.cpp` 要加入根 `CMakeLists.txt` 的 `HVNETPP_SOURCES`。
   - 新增测试或示例要加入 `examples/CMakeLists.txt` 的 `HVNETPP_EXAMPLES`。
   - 注释和日志用中文。
5. **验证**（必须在 Linux 下进行，macOS/Windows 本机无法编译）：
   - 基础构建与全部自测（unpack / loop / net_selftest）：`./script/build.sh -t Debug --test`
   - sanitizer：`./script/build.sh -c --asan --test`（或 `--ubsan`），再跑示例场景：`./script/run_examples.sh build-asan`
     （覆盖 TCP echo、断线重连、聊天室拆包广播、UDP 多线程 echo、timer_demo，并检查 sanitizer 报告）
   - 并发相关改动（队列、跨线程、start/stop 流程）：`./script/build.sh -c --tsan --test` 加 `./script/run_examples.sh build-tsan`。
     涉及时序的自测断言要用轮询等待，不要依赖固定 sleep（TSan 下会慢数倍）。
   - 性能相关改动：Release 构建后用 `build/examples/net_bench`（post / udp / timer）给出改动前后数据；
     基线要在改动前的代码上用同一环境、同一参数测。注意发送端是否先于接收端打满 CPU（看 send_total 与 recv 是否接近）。
   - 新增自测放进 `loop_selftest`（EventLoop/定时器/基础类型）或 `net_selftest`（UDP/TCP 端到端），二者都已接入 `--test`。
   - macOS 上用 Docker（已验证，Apple Silicon 上跑原生 arm64，顺带覆盖 ARM 弱内存序）：
     ```bash
     docker run -d --name hvnetpp-dev -v "$PWD":/src -w /src ubuntu:24.04 sleep infinity
     docker exec hvnetpp-dev bash -c 'apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq g++ cmake make'
     docker exec hvnetpp-dev bash -c 'cd /src && ./script/build.sh -t Debug --test'
     ```
     本机已有的 `cloudlongme/cplusplusbuildenv:v4`（openEuler 22.03，GCC 10，amd64 模拟运行）可用来补一遍 x86 + 旧内核头的编译检查，
     先把仓库拷到容器内再构建，避免与 arm64 的 build 目录互相覆盖。
     容器内生成的 build 目录与宿主机共享，CMakeCache 记录的是容器内路径，换到 WSL 或其他环境构建时加 `-c`。
6. **收尾**：
   - 在 backlog.md 中把条目状态改为"已完成（日期，commit 或简述）"。如果实际做法和建议不同，补一句说明。
   - 如果改变了约定或架构，同步更新根目录 CLAUDE.md 和 README（快速上手、线程模型、注意事项、"与旧版 API 的差异"）。
   - 没有用户要求不要提交 commit。

## SFU 热路径检查清单

评审或实现任何改动时逐条过一遍：

- **每包开销**：收/发一个 UDP 包要几次 malloc、几次 memcpy、几次系统调用？目标是批量 syscall（recvmmsg / sendmmsg / GSO）和零分配（包池或调用方提供缓冲）。
- **跨线程投递**：一次投递有几次分配？是否每次都写 eventfd？同一生产者的任务是否严格 FIFO？
- **锁**：热路径上有没有全局锁（包括日志级别检查）？锁内有没有 IO 或日志？
- **日志**：每包、每次投递的路径上不能有开启级别的同步日志；错误日志需要限频。
- **定时器**：是否每个会话、每次重臂都 new 一个定时器？能否用 loop 级 tick 或时间轮代替？
- **内存序**：acquire/release 是否成对？只在 x86 上"碰巧正确"不算正确，目标平台包括 ARM 服务器。
- **fd 生命周期**：其他线程读到的 fd 是否可能已被关闭并复用？Channel 是否会在自己的回调里被同步析构（必须先 `disableAll` + `remove`，再交给 `queueInLoop` 延迟释放）？
- **错误处理**：可恢复错误（单个 fd 的 epoll_ctl/bind 失败、EAGAIN、ECONNREFUSED、fd 耗尽）不能 abort 进程，也不能刷屏。
- **多核与亲和**：会话状态能否固定在一个 loop 上？信令、媒体、定时器是否在同一个线程？
