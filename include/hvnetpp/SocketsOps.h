#pragma once

#include <arpa/inet.h>
#include <sys/uio.h>

namespace hvnetpp {
namespace sockets {

// Non-fatal helpers. They log and preserve errno-style failure reporting.
int createNonblocking(sa_family_t family);
int createNonblockingUdp(sa_family_t family);
bool bind(int sockfd, const struct sockaddr* addr);
bool listen(int sockfd);

// Fatal convenience wrappers kept for low-level callers that explicitly want fail-fast behavior.
int createNonblockingOrDie(sa_family_t family);
int createNonblockingUdpOrDie(sa_family_t family);
int connect(int sockfd, const struct sockaddr* addr);
void bindOrDie(int sockfd, const struct sockaddr* addr);
void listenOrDie(int sockfd);
int accept(int sockfd, struct sockaddr_in6* addr);
ssize_t read(int sockfd, void *buf, size_t count);
ssize_t readv(int sockfd, const struct iovec *iov, int iovcnt);
ssize_t write(int sockfd, const void *buf, size_t count);
void close(int sockfd);
void shutdownWrite(int sockfd);

void toIpPort(char* buf, size_t size, const struct sockaddr* addr);
void toIp(char* buf, size_t size, const struct sockaddr* addr);
void fromIpPort(const char* ip, uint16_t port, struct sockaddr_in* addr);
void fromIpPort(const char* ip, uint16_t port, struct sockaddr_in6* addr);

int getSocketError(int sockfd);
const struct sockaddr* sockaddr_cast(const struct sockaddr_in* addr);
const struct sockaddr* sockaddr_cast(const struct sockaddr_in6* addr);
struct sockaddr* sockaddr_cast(struct sockaddr_in6* addr);
const struct sockaddr_in* sockaddr_in_cast(const struct sockaddr* addr);
const struct sockaddr_in6* sockaddr_in6_cast(const struct sockaddr* addr);

struct sockaddr_in6 getLocalAddr(int sockfd);
struct sockaddr_in6 getPeerAddr(int sockfd);
bool isSelfConnect(int sockfd);

// Socket options
void setTcpNoDelay(int sockfd, bool on);
void setReuseAddr(int sockfd, bool on);
void setReusePort(int sockfd, bool on);
void setKeepAlive(int sockfd, bool on);

} // namespace sockets
} // namespace hvnetpp
