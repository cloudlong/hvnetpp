#pragma once

#include <arpa/inet.h>
#include <cstring>
#include <sys/socket.h>
#include <string>
#include <netinet/in.h>

namespace hvnetpp {

// Value-type wrapper around a concrete socket address.
class InetAddress {
public:
    // Constructs an endpoint with given port number.
    // Mostly used in TcpServer listening.
    explicit InetAddress(uint16_t port = 0, bool loopbackOnly = false, bool ipv6 = false);

    // Constructs an endpoint with given IP literal and port.
    // `ipv6` selects whether `ip` is parsed as IPv4 or IPv6 text.
    // Invalid literals leave the address invalid; prefer tryParse() for user input.
    InetAddress(std::string ip, uint16_t port, bool ipv6 = false);

    // Constructs an endpoint with given struct @c sockaddr_in
    // Mostly used when accepting new connections
    explicit InetAddress(const struct sockaddr_in& addr)
        : length_(static_cast<socklen_t>(sizeof addr)) {
        std::memset(&addr_, 0, sizeof addr_);
        std::memcpy(&addr_, &addr, sizeof addr);
    }

    explicit InetAddress(const struct sockaddr_in6& addr)
        : length_(static_cast<socklen_t>(sizeof addr)) {
        std::memset(&addr_, 0, sizeof addr_);
        std::memcpy(&addr_, &addr, sizeof addr);
    }

    InetAddress(const struct sockaddr* addr, socklen_t len);

    static InetAddress loopback(uint16_t port, bool ipv6 = false);
    static InetAddress any(uint16_t port, bool ipv6 = false);
    // Parses a textual IP literal into `result`. Returns false on invalid input.
    static bool tryParse(const std::string& ip, uint16_t port, InetAddress* result, bool ipv6 = false);

    sa_family_t family() const { return addr_.ss_family; }
    bool isIpv4() const { return family() == AF_INET; }
    bool isIpv6() const { return family() == AF_INET6; }
    bool isValid() const { return length_ != 0 && (isIpv4() || isIpv6()); }
    std::string toIp() const;
    std::string toIpPort() const;
    uint16_t toPort() const;
    socklen_t length() const { return length_; }

    const struct sockaddr* getSockAddr() const { return reinterpret_cast<const struct sockaddr*>(&addr_); }

    uint32_t ipNetEndian() const;
    uint16_t portNetEndian() const;

    // Compatibility shim. Prefer using Resolver directly for new code.
    // If `error_code` is non-null it receives the getaddrinfo-style error code.
    static bool resolve(std::string hostname,
                        InetAddress* result,
                        uint16_t port = 0,
                        sa_family_t family = AF_UNSPEC,
                        int* error_code = nullptr);

private:
    struct sockaddr_storage addr_;
    socklen_t length_;
};

} // namespace hvnetpp
