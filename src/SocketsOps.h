#pragma once

#include "hvnetpp/InetAddress.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/uio.h>

namespace hvnetpp {
namespace detail {
namespace sockets {

int createNonblocking(sa_family_t family);
int createNonblockingUdp(sa_family_t family);
int connect(int sockfd, const struct sockaddr* addr);
bool bind(int sockfd, const InetAddress& addr);
bool listen(int sockfd);
int accept(int sockfd, struct sockaddr_storage* addr, socklen_t* addrlen);
ssize_t read(int sockfd, void* buf, size_t count);
ssize_t readv(int sockfd, const struct iovec* iov, int iovcnt);
ssize_t write(int sockfd, const void* buf, size_t count);
void close(int sockfd);
void shutdownWrite(int sockfd);

void toIpPort(char* buf, size_t size, const struct sockaddr* addr);
void toIp(char* buf, size_t size, const struct sockaddr* addr);
bool fromIpPort(const char* ip, uint16_t port, struct sockaddr_in* addr);
bool fromIpPort(const char* ip, uint16_t port, struct sockaddr_in6* addr);

int getSocketError(int sockfd);
InetAddress getLocalAddr(int sockfd);
InetAddress getPeerAddr(int sockfd);
bool isSelfConnect(int sockfd);

void setTcpNoDelay(int sockfd, bool on);
void setReuseAddr(int sockfd, bool on);
void setReusePort(int sockfd, bool on);
void setKeepAlive(int sockfd, bool on);

} // namespace sockets
} // namespace detail
} // namespace hvnetpp
