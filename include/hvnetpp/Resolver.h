#pragma once

#include <netinet/in.h>
#include <string>
#include <sys/socket.h>

namespace hvnetpp {

class InetAddress;

class Resolver {
public:
    // Resolves `hostname` into a concrete address with explicit port/family.
    // Returns true on success. If `error_code` is non-null it receives the getaddrinfo-style error code.
    static bool resolve(const std::string& hostname,
                        InetAddress* result,
                        uint16_t port = 0,
                        sa_family_t family = AF_UNSPEC,
                        int socktype = SOCK_STREAM,
                        int* error_code = nullptr);
    // Returns a human-readable string for a getaddrinfo-style error code.
    static const char* errorString(int error_code);
};

} // namespace hvnetpp
