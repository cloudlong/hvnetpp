#pragma once

// 内部：UDP fd 级别的操作，UdpSocket / UdpServer / UdpClient 共用
#include "hvnetpp/UdpSocket.h"

namespace hvnetpp {
namespace udpio {

// 对 fd 应用选项（reusePort 除外）；单个选项失败只打 WARN，不中断
void applyOptions(int fd, sa_family_t family, const UdpSocketOptions& opts);

// 创建非阻塞 UDP socket、应用选项并 bind；reusePort 为 true 时 bind 前设置 SO_REUSEPORT。
// 失败返回 -1（errno 保留）
int createBound(const InetAddress& addr, const UdpSocketOptions& opts, bool reusePort);

// family 为 socket 的地址族（决定 PKTINFO 用 IPv4 还是 IPv6 的 cmsg）；peer 为空表示 connected socket
ssize_t sendTo(int fd, sa_family_t family, const void* data, size_t len,
               const InetAddress* peer, const InetAddress* local);
int sendBatch(int fd, sa_family_t family, const UdpSendItem* items, size_t n);

} // namespace udpio
} // namespace hvnetpp
