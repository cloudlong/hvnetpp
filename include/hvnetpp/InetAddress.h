#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <netinet/in.h>
#include <sys/socket.h>

namespace hvnetpp {

// Wrapper of sockaddr_in.
// This is an POD interface class.
class InetAddress {
public:
    // Constructs an endpoint with given port number.
    // Mostly used in TcpServer listening.
    explicit InetAddress(uint16_t port = 0, bool loopbackOnly = false, bool ipv6 = false);

    // Constructs an endpoint with given ip and port.
    // @c ip should be "1.2.3.4"
    InetAddress(std::string ip, uint16_t port, bool ipv6 = false);

    // Constructs an endpoint with given struct @c sockaddr_in
    // Mostly used when accepting new connections
    explicit InetAddress(const struct sockaddr_in& addr)
        : addr_(addr) {}

    explicit InetAddress(const struct sockaddr_in6& addr)
        : addr6_(addr) {}

    sa_family_t family() const { return addr_.sin_family; }
    // 按 family 返回 sockaddr 长度，可直接传给 sendto/bind/connect
    socklen_t socklen() const {
        return static_cast<socklen_t>(family() == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
    }
    std::string toIp() const;
    std::string toIpPort() const;
    uint16_t toPort() const;

    const struct sockaddr* getSockAddr() const { return reinterpret_cast<const struct sockaddr*>(&addr6_); }
    void setSockAddr6(const struct sockaddr_in6& addr6) { addr6_ = addr6; }

    uint32_t ipNetEndian() const;
    uint16_t portNetEndian() const { return addr_.sin_port; }

    // resolve hostname to IP address, not changing port or sin_family
    // return true on success.
    // thread safe
    static bool resolve(std::string hostname, InetAddress* result);

    // 比较 family + IP + 端口（IPv6 另比较 scope_id），不分配内存。
    // 注意 IPv4 地址与其 v4-mapped IPv6 形式（::ffff:a.b.c.d）视为不同。
    bool operator==(const InetAddress& rhs) const {
        if (family() != rhs.family()) {
            return false;
        }
        if (family() == AF_INET) {
            return addr_.sin_port == rhs.addr_.sin_port && addr_.sin_addr.s_addr == rhs.addr_.sin_addr.s_addr;
        }
        if (family() == AF_INET6) {
            return addr6_.sin6_port == rhs.addr6_.sin6_port &&
                   addr6_.sin6_scope_id == rhs.addr6_.sin6_scope_id &&
                   std::memcmp(&addr6_.sin6_addr, &rhs.addr6_.sin6_addr, sizeof addr6_.sin6_addr) == 0;
        }
        return true;
    }
    bool operator!=(const InetAddress& rhs) const { return !(*this == rhs); }

    // 与 operator== 一致的哈希，可直接作为 std::unordered_map 的 key（按五元组查会话）
    size_t hash() const {
        uint64_t h;
        if (family() == AF_INET6) {
            uint64_t hi, lo;
            std::memcpy(&hi, &addr6_.sin6_addr, 8);
            std::memcpy(&lo, reinterpret_cast<const char*>(&addr6_.sin6_addr) + 8, 8);
            h = hi * 0x9E3779B97F4A7C15ULL ^ lo ^ (static_cast<uint64_t>(addr6_.sin6_port) << 48) ^
                static_cast<uint64_t>(addr6_.sin6_scope_id) << 16;
        } else {
            h = (static_cast<uint64_t>(addr_.sin_addr.s_addr) << 16) ^ addr_.sin_port ^
                (static_cast<uint64_t>(family()) << 56);
        }
        // splitmix64 finalizer
        h ^= h >> 30;
        h *= 0xBF58476D1CE4E5B9ULL;
        h ^= h >> 27;
        h *= 0x94D049BB133111EBULL;
        h ^= h >> 31;
        return static_cast<size_t>(h);
    }

private:
    union {
        struct sockaddr_in addr_;
        struct sockaddr_in6 addr6_;
    };
};

} // namespace hvnetpp

namespace std {
template <>
struct hash<hvnetpp::InetAddress> {
    size_t operator()(const hvnetpp::InetAddress& addr) const { return addr.hash(); }
};
} // namespace std
