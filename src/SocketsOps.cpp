#include "SocketsOps.h"

#include "rtclog.h"

#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

namespace hvnetpp {
namespace detail {
namespace sockets {

namespace {

socklen_t sockaddrLength(const struct sockaddr* addr) {
    return static_cast<socklen_t>(
        addr->sa_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
}

} // namespace

int createNonblocking(sa_family_t family) {
    int sockfd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (sockfd < 0) {
        RTCLOG(RTC_ERROR, "detail::sockets::createNonblocking error: %s", strerror(errno));
    }
    return sockfd;
}

int createNonblockingUdp(sa_family_t family) {
    int sockfd = ::socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
    if (sockfd < 0) {
        RTCLOG(RTC_ERROR, "detail::sockets::createNonblockingUdp error: %s", strerror(errno));
    }
    return sockfd;
}

bool bind(int sockfd, const InetAddress& addr) {
    if (::bind(sockfd, addr.getSockAddr(), addr.length()) < 0) {
        RTCLOG(RTC_ERROR, "detail::sockets::bind error: %s", strerror(errno));
        return false;
    }
    return true;
}

bool listen(int sockfd) {
    if (::listen(sockfd, SOMAXCONN) < 0) {
        RTCLOG(RTC_ERROR, "detail::sockets::listen error: %s", strerror(errno));
        return false;
    }
    return true;
}

int accept(int sockfd, struct sockaddr_storage* addr, socklen_t* addrlen) {
    int connfd = ::accept4(sockfd,
                           reinterpret_cast<struct sockaddr*>(addr),
                           addrlen,
                           SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (connfd < 0) {
        const int savedErrno = errno;
        switch (savedErrno) {
            case EAGAIN:
            case ECONNABORTED:
            case EINTR:
            case EPROTO:
            case EPERM:
            case EMFILE:
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
                RTCLOG(RTC_ERROR, "detail::sockets::accept error: %s", strerror(savedErrno));
                RTCLOG(RTC_FATAL, "unexpected error of ::accept %d", savedErrno);
                abort();
                break;
            default:
                RTCLOG(RTC_ERROR, "detail::sockets::accept error: %s", strerror(savedErrno));
                RTCLOG(RTC_FATAL, "unknown error of ::accept %d", savedErrno);
                abort();
                break;
        }
    }
    return connfd;
}

int connect(int sockfd, const struct sockaddr* addr) {
    return ::connect(sockfd, addr, sockaddrLength(addr));
}

ssize_t read(int sockfd, void* buf, size_t count) {
    return ::read(sockfd, buf, count);
}

ssize_t readv(int sockfd, const struct iovec* iov, int iovcnt) {
    return ::readv(sockfd, iov, iovcnt);
}

ssize_t write(int sockfd, const void* buf, size_t count) {
    return ::write(sockfd, buf, count);
}

void close(int sockfd) {
    if (::close(sockfd) < 0) {
        RTCLOG(RTC_ERROR, "detail::sockets::close error: %s", strerror(errno));
    }
}

void shutdownWrite(int sockfd) {
    if (::shutdown(sockfd, SHUT_WR) < 0) {
        RTCLOG(RTC_ERROR, "detail::sockets::shutdownWrite error: %s", strerror(errno));
    }
}

void toIpPort(char* buf, size_t size, const struct sockaddr* addr) {
    char ip[INET6_ADDRSTRLEN] = "";
    toIp(ip, sizeof ip, addr);
    uint16_t port = 0;
    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in* addr4 = reinterpret_cast<const struct sockaddr_in*>(addr);
        port = ntohs(addr4->sin_port);
        snprintf(buf, size, "%s:%u", ip, port);
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6* addr6 = reinterpret_cast<const struct sockaddr_in6*>(addr);
        port = ntohs(addr6->sin6_port);
        snprintf(buf, size, "[%s]:%u", ip, port);
    } else if (size > 0) {
        buf[0] = '\0';
    }
}

void toIp(char* buf, size_t size, const struct sockaddr* addr) {
    if (addr->sa_family == AF_INET) {
        assert(size >= INET_ADDRSTRLEN);
        const struct sockaddr_in* addr4 = reinterpret_cast<const struct sockaddr_in*>(addr);
        ::inet_ntop(AF_INET, &addr4->sin_addr, buf, static_cast<socklen_t>(size));
    } else if (addr->sa_family == AF_INET6) {
        assert(size >= INET6_ADDRSTRLEN);
        const struct sockaddr_in6* addr6 = reinterpret_cast<const struct sockaddr_in6*>(addr);
        ::inet_ntop(AF_INET6, &addr6->sin6_addr, buf, static_cast<socklen_t>(size));
    } else if (size > 0) {
        buf[0] = '\0';
    }
}

bool fromIpPort(const char* ip, uint16_t port, struct sockaddr_in* addr) {
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    const int ret = ::inet_pton(AF_INET, ip, &addr->sin_addr);
    if (ret == 0) {
        RTCLOG(RTC_ERROR, "detail::sockets::fromIpPort invalid IPv4 address: %s", ip);
        return false;
    }
    if (ret < 0) {
        RTCLOG(RTC_ERROR, "detail::sockets::fromIpPort error: %s", strerror(errno));
        return false;
    }
    return true;
}

bool fromIpPort(const char* ip, uint16_t port, struct sockaddr_in6* addr) {
    addr->sin6_family = AF_INET6;
    addr->sin6_port = htons(port);
    const int ret = ::inet_pton(AF_INET6, ip, &addr->sin6_addr);
    if (ret == 0) {
        RTCLOG(RTC_ERROR, "detail::sockets::fromIpPort invalid IPv6 address: %s", ip);
        return false;
    }
    if (ret < 0) {
        RTCLOG(RTC_ERROR, "detail::sockets::fromIpPort error: %s", strerror(errno));
        return false;
    }
    return true;
}

int getSocketError(int sockfd) {
    int optval;
    socklen_t optlen = static_cast<socklen_t>(sizeof optval);
    if (::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0) {
        return errno;
    }
    return optval;
}

InetAddress getLocalAddr(int sockfd) {
    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof addr);
    socklen_t addrlen = static_cast<socklen_t>(sizeof addr);
    if (::getsockname(sockfd, reinterpret_cast<struct sockaddr*>(&addr), &addrlen) < 0) {
        RTCLOG(RTC_ERROR, "detail::sockets::getLocalAddr error: %s", strerror(errno));
    }
    return InetAddress(reinterpret_cast<const struct sockaddr*>(&addr), addrlen);
}

InetAddress getPeerAddr(int sockfd) {
    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof addr);
    socklen_t addrlen = static_cast<socklen_t>(sizeof addr);
    if (::getpeername(sockfd, reinterpret_cast<struct sockaddr*>(&addr), &addrlen) < 0) {
        RTCLOG(RTC_ERROR, "detail::sockets::getPeerAddr error: %s", strerror(errno));
    }
    return InetAddress(reinterpret_cast<const struct sockaddr*>(&addr), addrlen);
}

bool isSelfConnect(int sockfd) {
    InetAddress localaddr = getLocalAddr(sockfd);
    InetAddress peeraddr = getPeerAddr(sockfd);
    if (localaddr.isIpv4() && peeraddr.isIpv4()) {
        const struct sockaddr_in* laddr4 =
            reinterpret_cast<const struct sockaddr_in*>(localaddr.getSockAddr());
        const struct sockaddr_in* raddr4 =
            reinterpret_cast<const struct sockaddr_in*>(peeraddr.getSockAddr());
        return laddr4->sin_port == raddr4->sin_port
            && laddr4->sin_addr.s_addr == raddr4->sin_addr.s_addr;
    }
    if (localaddr.isIpv6() && peeraddr.isIpv6()) {
        const struct sockaddr_in6* laddr6 =
            reinterpret_cast<const struct sockaddr_in6*>(localaddr.getSockAddr());
        const struct sockaddr_in6* raddr6 =
            reinterpret_cast<const struct sockaddr_in6*>(peeraddr.getSockAddr());
        return laddr6->sin6_port == raddr6->sin6_port
            && memcmp(&laddr6->sin6_addr, &raddr6->sin6_addr, sizeof laddr6->sin6_addr) == 0;
    }
    return false;
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
} // namespace detail
} // namespace hvnetpp
