#include "hvnetpp/InetAddress.h"
#include "hvnetpp/SocketsOps.h"
#include <netdb.h>
#include <sys/socket.h>
#include <strings.h>
#include <cstring>
#include <cstddef>
#include <cassert>
#include <arpa/inet.h>

// INADDR_ANY use (type)value casting.
#pragma GCC diagnostic ignored "-Wold-style-cast"
static const in_addr_t kInaddrAny = INADDR_ANY;
static const in_addr_t kInaddrLoopback = INADDR_LOOPBACK;
#pragma GCC diagnostic error "-Wold-style-cast"

namespace hvnetpp {

static_assert(sizeof(InetAddress) == sizeof(struct sockaddr_in6), "InetAddress is same size as sockaddr_in6");
static_assert(offsetof(sockaddr_in, sin_family) == 0, "sin_family offset 0");
static_assert(offsetof(sockaddr_in6, sin6_family) == 0, "sin6_family offset 0");
static_assert(offsetof(sockaddr_in, sin_port) == 2, "sin_port offset 2");
static_assert(offsetof(sockaddr_in6, sin6_port) == 2, "sin6_port offset 2");

InetAddress::InetAddress(uint16_t port, bool loopbackOnly, bool ipv6) {
    if (ipv6) {
        bzero(&addr6_, sizeof addr6_);
        addr6_.sin6_family = AF_INET6;
        in6_addr ip = loopbackOnly ? in6addr_loopback : in6addr_any;
        addr6_.sin6_addr = ip;
        addr6_.sin6_port = htons(port);
    } else {
        bzero(&addr_, sizeof addr_);
        addr_.sin_family = AF_INET;
        in_addr_t ip = loopbackOnly ? kInaddrLoopback : kInaddrAny;
        addr_.sin_addr.s_addr = htonl(ip);
        addr_.sin_port = htons(port);
    }
}

InetAddress::InetAddress(std::string ip, uint16_t port, bool ipv6) {
    if (ipv6) {
        bzero(&addr6_, sizeof addr6_);
        sockets::fromIpPort(ip.c_str(), port, &addr6_);
    } else {
        bzero(&addr_, sizeof addr_);
        sockets::fromIpPort(ip.c_str(), port, &addr_);
    }
}

std::string InetAddress::toIpPort() const {
    char buf[64] = "";
    sockets::toIpPort(buf, sizeof buf, getSockAddr());
    return buf;
}

std::string InetAddress::toIp() const {
    char buf[64] = "";
    sockets::toIp(buf, sizeof buf, getSockAddr());
    return buf;
}

uint32_t InetAddress::ipNetEndian() const {
    assert(family() == AF_INET);
    return addr_.sin_addr.s_addr;
}

uint16_t InetAddress::toPort() const {
    return ntohs(portNetEndian());
}

bool InetAddress::resolve(std::string hostname, InetAddress* out, uint16_t port, sa_family_t family) {
    assert(out != NULL);

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = (family == AF_INET || family == AF_INET6) ? family : AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = NULL;
    const int ret = ::getaddrinfo(hostname.c_str(), NULL, &hints, &result);
    if (ret != 0 || result == NULL) {
        return false;
    }

    bool resolved = false;
    const uint16_t portNetEndian = htons(port);
    for (struct addrinfo* ai = result; ai != NULL; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            const struct sockaddr_in* addr4 =
                reinterpret_cast<const struct sockaddr_in*>(ai->ai_addr);
            out->addr_ = *addr4;
            out->addr_.sin_port = portNetEndian;
            resolved = true;
            break;
        }
        if (ai->ai_family == AF_INET6) {
            const struct sockaddr_in6* addr6 =
                reinterpret_cast<const struct sockaddr_in6*>(ai->ai_addr);
            out->addr6_ = *addr6;
            out->addr6_.sin6_port = portNetEndian;
            resolved = true;
            break;
        }
    }

    ::freeaddrinfo(result);
    return resolved;
}

} // namespace hvnetpp
