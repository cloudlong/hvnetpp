#include "hvnetpp/SocketsOps.h"
#include "rtclog.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <assert.h>
#include <cstdlib>
#include <cstring>

namespace hvnetpp {
namespace sockets {

using SA = struct sockaddr;

namespace {

socklen_t sockaddrLength(const struct sockaddr* addr) {
    return static_cast<socklen_t>(
        addr->sa_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
}

} // namespace

const SA* sockaddr_cast(const struct sockaddr_in* addr) {
    return static_cast<const SA*>(static_cast<const void*>(addr));
}

const SA* sockaddr_cast(const struct sockaddr_in6* addr) {
    return static_cast<const SA*>(static_cast<const void*>(addr));
}

SA* sockaddr_cast(struct sockaddr_in6* addr) {
    return static_cast<SA*>(static_cast<void*>(addr));
}

const struct sockaddr_in* sockaddr_in_cast(const struct sockaddr* addr) {
    return static_cast<const struct sockaddr_in*>(static_cast<const void*>(addr));
}

const struct sockaddr_in6* sockaddr_in6_cast(const struct sockaddr* addr) {
    return static_cast<const struct sockaddr_in6*>(static_cast<const void*>(addr));
}

int createNonblocking(sa_family_t family) {
    int sockfd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (sockfd < 0) {
        RTCLOG(RTC_ERROR, "sockets::createNonblocking error: %s", strerror(errno));
    }
    return sockfd;
}

int createNonblockingUdp(sa_family_t family) {
    int sockfd = ::socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
    if (sockfd < 0) {
        RTCLOG(RTC_ERROR, "sockets::createNonblockingUdp error: %s", strerror(errno));
    }
    return sockfd;
}

int createNonblockingOrDie(sa_family_t family) {
    int sockfd = createNonblocking(family);
    if (sockfd < 0) {
        RTCLOG(RTC_FATAL, "sockets::createNonblockingOrDie");
        abort();
    }
    return sockfd;
}

int createNonblockingUdpOrDie(sa_family_t family) {
    int sockfd = createNonblockingUdp(family);
    if (sockfd < 0) {
        RTCLOG(RTC_FATAL, "sockets::createNonblockingUdpOrDie");
        abort();
    }
    return sockfd;
}

bool bind(int sockfd, const struct sockaddr* addr) {
    int ret = ::bind(sockfd, addr, sockaddrLength(addr));
    if (ret < 0) {
        RTCLOG(RTC_ERROR, "sockets::bind error: %s", strerror(errno));
        return false;
    }
    return true;
}

void bindOrDie(int sockfd, const struct sockaddr* addr) {
    if (!bind(sockfd, addr)) {
        RTCLOG(RTC_FATAL, "sockets::bindOrDie");
        abort();
    }
}

bool listen(int sockfd) {
    int ret = ::listen(sockfd, SOMAXCONN);
    if (ret < 0) {
        RTCLOG(RTC_ERROR, "sockets::listen error: %s", strerror(errno));
        return false;
    }
    return true;
}

void listenOrDie(int sockfd) {
    if (!listen(sockfd)) {
        RTCLOG(RTC_FATAL, "sockets::listenOrDie");
        abort();
    }
}

int accept(int sockfd, struct sockaddr_in6* addr) {
    socklen_t addrlen = static_cast<socklen_t>(sizeof *addr);
    int connfd = ::accept4(sockfd, sockaddr_cast(addr),
                           &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (connfd < 0) {
        int savedErrno = errno;
        switch (savedErrno) {
            case EAGAIN:
            case ECONNABORTED:
            case EINTR:
            case EPROTO: // ???
            case EPERM:
            case EMFILE: // per-process lmit of open file desctiptor ???
                // expected errors
                errno = savedErrno;
                break;
            case EBADF:
            case EFAULT:
            case EINVAL:
            case ENFILE:
            case ENOBUFS:
            case ENOMEM:
            case ENOTSOCK:
            case EOPNOTSUPP:
                // unexpected errors
                RTCLOG(RTC_ERROR, "Socket::accept error: %s", strerror(savedErrno));
                RTCLOG(RTC_FATAL, "unexpected error of ::accept %d", savedErrno);
                break;
            default:
                RTCLOG(RTC_ERROR, "Socket::accept error: %s", strerror(savedErrno));
                RTCLOG(RTC_FATAL, "unknown error of ::accept %d", savedErrno);
                break;
        }
    }
    return connfd;
}

int connect(int sockfd, const struct sockaddr* addr) {
    return ::connect(sockfd, addr, sockaddrLength(addr));
}

ssize_t read(int sockfd, void *buf, size_t count) {
    return ::read(sockfd, buf, count);
}

ssize_t readv(int sockfd, const struct iovec *iov, int iovcnt) {
    return ::readv(sockfd, iov, iovcnt);
}

ssize_t write(int sockfd, const void *buf, size_t count) {
    return ::write(sockfd, buf, count);
}

void close(int sockfd) {
    if (::close(sockfd) < 0) {
        RTCLOG(RTC_ERROR, "sockets::close error: %s", strerror(errno));
    }
}

void shutdownWrite(int sockfd) {
    if (::shutdown(sockfd, SHUT_WR) < 0) {
        RTCLOG(RTC_ERROR, "sockets::shutdownWrite error: %s", strerror(errno));
    }
}

void toIpPort(char* buf, size_t size, const struct sockaddr* addr) {
    char ip[INET6_ADDRSTRLEN] = "";
    toIp(ip, sizeof ip, addr);
    uint16_t port = 0;
    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in* addr4 = sockaddr_in_cast(addr);
        port = ntohs(addr4->sin_port);
        snprintf(buf, size, "%s:%u", ip, port);
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6* addr6 = sockaddr_in6_cast(addr);
        port = ntohs(addr6->sin6_port);
        snprintf(buf, size, "[%s]:%u", ip, port);
    } else if (size > 0) {
        buf[0] = '\0';
    }
}

void toIp(char* buf, size_t size, const struct sockaddr* addr) {
    if (addr->sa_family == AF_INET) {
        assert(size >= INET_ADDRSTRLEN);
        const struct sockaddr_in* addr4 = sockaddr_in_cast(addr);
        ::inet_ntop(AF_INET, &addr4->sin_addr, buf, static_cast<socklen_t>(size));
    } else if (addr->sa_family == AF_INET6) {
        assert(size >= INET6_ADDRSTRLEN);
        const struct sockaddr_in6* addr6 = sockaddr_in6_cast(addr);
        ::inet_ntop(AF_INET6, &addr6->sin6_addr, buf, static_cast<socklen_t>(size));
    }
}

void fromIpPort(const char* ip, uint16_t port, struct sockaddr_in* addr) {
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    int ret = ::inet_pton(AF_INET, ip, &addr->sin_addr);
    if (ret == 0) {
        RTCLOG(RTC_ERROR, "sockets::fromIpPort invalid IPv4 address: %s", ip);
    } else if (ret < 0) {
        RTCLOG(RTC_ERROR, "sockets::fromIpPort error: %s", strerror(errno));
    }
}

void fromIpPort(const char* ip, uint16_t port, struct sockaddr_in6* addr) {
    addr->sin6_family = AF_INET6;
    addr->sin6_port = htons(port);
    int ret = ::inet_pton(AF_INET6, ip, &addr->sin6_addr);
    if (ret == 0) {
        RTCLOG(RTC_ERROR, "sockets::fromIpPort invalid IPv6 address: %s", ip);
    } else if (ret < 0) {
        RTCLOG(RTC_ERROR, "sockets::fromIpPort error: %s", strerror(errno));
    }
}

int getSocketError(int sockfd) {
    int optval;
    socklen_t optlen = static_cast<socklen_t>(sizeof optval);
    if (::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0) {
        return errno;
    } else {
        return optval;
    }
}

struct sockaddr_in6 getLocalAddr(int sockfd) {
    struct sockaddr_in6 localaddr;
    bzero(&localaddr, sizeof localaddr);
    socklen_t addrlen = static_cast<socklen_t>(sizeof localaddr);
    if (::getsockname(sockfd, sockaddr_cast(&localaddr), &addrlen) < 0) {
        RTCLOG(RTC_ERROR, "sockets::getLocalAddr error: %s", strerror(errno));
    }
    return localaddr;
}

struct sockaddr_in6 getPeerAddr(int sockfd) {
    struct sockaddr_in6 peeraddr;
    bzero(&peeraddr, sizeof peeraddr);
    socklen_t addrlen = static_cast<socklen_t>(sizeof peeraddr);
    if (::getpeername(sockfd, sockaddr_cast(&peeraddr), &addrlen) < 0) {
        RTCLOG(RTC_ERROR, "sockets::getPeerAddr error: %s", strerror(errno));
    }
    return peeraddr;
}

bool isSelfConnect(int sockfd) {
    struct sockaddr_in6 localaddr = getLocalAddr(sockfd);
    struct sockaddr_in6 peeraddr = getPeerAddr(sockfd);
    if (localaddr.sin6_family == AF_INET) {
        const struct sockaddr_in* laddr4 = reinterpret_cast<struct sockaddr_in*>(&localaddr);
        const struct sockaddr_in* raddr4 = reinterpret_cast<struct sockaddr_in*>(&peeraddr);
        return laddr4->sin_port == raddr4->sin_port
            && laddr4->sin_addr.s_addr == raddr4->sin_addr.s_addr;
    } else if (localaddr.sin6_family == AF_INET6) {
        return localaddr.sin6_port == peeraddr.sin6_port
            && memcmp(&localaddr.sin6_addr, &peeraddr.sin6_addr, sizeof localaddr.sin6_addr) == 0;
    } else {
        return false;
    }
}

void setTcpNoDelay(int sockfd, bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &optval, static_cast<socklen_t>(sizeof optval));
}

void setReuseAddr(int sockfd, bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, static_cast<socklen_t>(sizeof optval));
}

void setReusePort(int sockfd, bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &optval, static_cast<socklen_t>(sizeof optval));
}

void setKeepAlive(int sockfd, bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &optval, static_cast<socklen_t>(sizeof optval));
}

} // namespace sockets
} // namespace hvnetpp
