#include "hvnetpp/InetAddress.h"
#include "hvnetpp/Resolver.h"
#include "SocketsOps.h"

#include <cstring>
#include <cassert>

namespace hvnetpp {

InetAddress::InetAddress(uint16_t port, bool loopbackOnly, bool ipv6) {
    std::memset(&addr_, 0, sizeof addr_);
    if (ipv6) {
        struct sockaddr_in6 addr6;
        std::memset(&addr6, 0, sizeof addr6);
        addr6.sin6_family = AF_INET6;
        in6_addr ip = loopbackOnly ? in6addr_loopback : in6addr_any;
        addr6.sin6_addr = ip;
        addr6.sin6_port = htons(port);
        std::memcpy(&addr_, &addr6, sizeof addr6);
        length_ = static_cast<socklen_t>(sizeof addr6);
    } else {
        struct sockaddr_in addr4;
        std::memset(&addr4, 0, sizeof addr4);
        addr4.sin_family = AF_INET;
        addr4.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
        addr4.sin_port = htons(port);
        std::memcpy(&addr_, &addr4, sizeof addr4);
        length_ = static_cast<socklen_t>(sizeof addr4);
    }
}

InetAddress::InetAddress(std::string ip, uint16_t port, bool ipv6)
    : length_(0) {
    std::memset(&addr_, 0, sizeof addr_);
    if (ipv6) {
        struct sockaddr_in6 addr6;
        std::memset(&addr6, 0, sizeof addr6);
        if (detail::sockets::fromIpPort(ip.c_str(), port, &addr6)) {
            std::memcpy(&addr_, &addr6, sizeof addr6);
            length_ = static_cast<socklen_t>(sizeof addr6);
        }
    } else {
        struct sockaddr_in addr4;
        std::memset(&addr4, 0, sizeof addr4);
        if (detail::sockets::fromIpPort(ip.c_str(), port, &addr4)) {
            std::memcpy(&addr_, &addr4, sizeof addr4);
            length_ = static_cast<socklen_t>(sizeof addr4);
        }
    }
}

InetAddress::InetAddress(const struct sockaddr* addr, socklen_t len)
    : length_(len) {
    std::memset(&addr_, 0, sizeof addr_);
    const socklen_t copyLen = len < static_cast<socklen_t>(sizeof addr_) ? len : static_cast<socklen_t>(sizeof addr_);
    std::memcpy(&addr_, addr, copyLen);
}

InetAddress InetAddress::loopback(uint16_t port, bool ipv6) {
    return InetAddress(port, true, ipv6);
}

InetAddress InetAddress::any(uint16_t port, bool ipv6) {
    return InetAddress(port, false, ipv6);
}

bool InetAddress::tryParse(const std::string& ip, uint16_t port, InetAddress* result, bool ipv6) {
    assert(result != NULL);
    *result = InetAddress(ip, port, ipv6);
    return result->isValid();
}

std::string InetAddress::toIpPort() const {
    char buf[64] = "";
    detail::sockets::toIpPort(buf, sizeof buf, getSockAddr());
    return buf;
}

std::string InetAddress::toIp() const {
    char buf[64] = "";
    detail::sockets::toIp(buf, sizeof buf, getSockAddr());
    return buf;
}

uint32_t InetAddress::ipNetEndian() const {
    assert(isIpv4());
    return reinterpret_cast<const struct sockaddr_in*>(&addr_)->sin_addr.s_addr;
}

uint16_t InetAddress::portNetEndian() const {
    if (isIpv6()) {
        return reinterpret_cast<const struct sockaddr_in6*>(&addr_)->sin6_port;
    }
    return reinterpret_cast<const struct sockaddr_in*>(&addr_)->sin_port;
}

uint16_t InetAddress::toPort() const {
    return ntohs(portNetEndian());
}

bool InetAddress::resolve(std::string hostname,
                          InetAddress* out,
                          uint16_t port,
                          sa_family_t family,
                          int* error_code) {
    return Resolver::resolve(hostname, out, port, family, SOCK_STREAM, error_code);
}

} // namespace hvnetpp
