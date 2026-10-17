#include "net.hpp"

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>
#endif

namespace bridge {

int close_sock(sock_t s) {
#ifdef _WIN32
    return closesocket(s);
#else
    return ::close(s);
#endif
}

void nonblock(sock_t s) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

int poll_socks(pollfd* fds, size_t n, int timeout_ms) {
#ifdef _WIN32
    return WSAPoll(fds, (ULONG)n, timeout_ms);
#else
    return ::poll(fds, (nfds_t)n, timeout_ms);
#endif
}

std::string failed(const char* what) {
#ifdef _WIN32
    int code = WSAGetLastError();
#else
    int code = errno;
#endif
    return std::string(what) + " failed (" + std::to_string(code) + ")";
}

std::vector<Iface> local_interfaces() {
    std::vector<Iface> out;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<uint8_t> mem(size);
    auto* list = (IP_ADAPTER_ADDRESSES*)mem.data();
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size) == ERROR_BUFFER_OVERFLOW) {
        mem.resize(size);
        list = (IP_ADAPTER_ADDRESSES*)mem.data();
        if (GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size) != NO_ERROR) return out;
    }
    for (auto* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sin = (sockaddr_in*)u->Address.lpSockaddr;
            if (sin->sin_family != AF_INET) continue;
            ULONG mask = 0;
            ConvertLengthToIpv4Mask(u->OnLinkPrefixLength, &mask);
            out.push_back({ sin->sin_addr.s_addr, (uint32_t)mask });
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (auto* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
        uint32_t mask = a->ifa_netmask ? ((sockaddr_in*)a->ifa_netmask)->sin_addr.s_addr : 0;
        out.push_back({ ((sockaddr_in*)a->ifa_addr)->sin_addr.s_addr, mask });
    }
    freeifaddrs(list);
#endif
    return out;
}

Waker::Waker() {
    sock_t w = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    if (w != kBadSock && ::bind(w, (sockaddr*)&a, sizeof a) == 0 && getsockname(w, (sockaddr*)&a, &len) == 0 &&
        ::connect(w, (sockaddr*)&a, sizeof a) == 0) {
        nonblock(w);
        fd_ = w;
    } else if (w != kBadSock) close_sock(w);
}

Waker::~Waker() {
    if (fd_ != kBadSock) close_sock(fd_);
}

void Waker::wake() {
    if (fd_ != kBadSock) ::send(fd_, "w", 1, 0);
}

void Waker::drain() {
    char c[16];
    while (fd_ != kBadSock && ::recv(fd_, c, sizeof c, 0) > 0) {}
}

}
