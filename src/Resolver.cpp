#include "hvnetpp/Resolver.h"

#include "hvnetpp/InetAddress.h"

#include <cassert>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>

namespace hvnetpp {

bool Resolver::resolve(const std::string& hostname,
                       InetAddress* result,
                       uint16_t port,
                       sa_family_t family,
                       int socktype,
                       int* error_code) {
    assert(result != NULL);
    if (error_code != NULL) {
        *error_code = 0;
    }

    if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6) {
        if (error_code != NULL) {
            *error_code = EAI_FAMILY;
        }
        return false;
    }

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof hints);
    hints.ai_family = family;
    hints.ai_socktype = socktype;

    struct addrinfo* resolved = NULL;
    const int ret = ::getaddrinfo(hostname.c_str(), NULL, &hints, &resolved);
    if (ret != 0 || resolved == NULL) {
        if (error_code != NULL) {
            *error_code = ret != 0 ? ret : EAI_NONAME;
        }
        return false;
    }

    bool ok = false;
    const uint16_t portNetEndian = htons(port);
    for (struct addrinfo* ai = resolved; ai != NULL; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET && ai->ai_addrlen >= static_cast<socklen_t>(sizeof(struct sockaddr_in))) {
            struct sockaddr_in addr4 =
                *reinterpret_cast<const struct sockaddr_in*>(ai->ai_addr);
            addr4.sin_port = portNetEndian;
            *result = InetAddress(addr4);
            ok = true;
            break;
        }
        if (ai->ai_family == AF_INET6 && ai->ai_addrlen >= static_cast<socklen_t>(sizeof(struct sockaddr_in6))) {
            struct sockaddr_in6 addr6 =
                *reinterpret_cast<const struct sockaddr_in6*>(ai->ai_addr);
            addr6.sin6_port = portNetEndian;
            *result = InetAddress(addr6);
            ok = true;
            break;
        }
    }

    ::freeaddrinfo(resolved);
    if (!ok && error_code != NULL) {
        *error_code = EAI_NONAME;
    }
    return ok;
}

const char* Resolver::errorString(int error_code) {
    return ::gai_strerror(error_code);
}

} // namespace hvnetpp
