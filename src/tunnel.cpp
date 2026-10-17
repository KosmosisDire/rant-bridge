#include "tunnel.hpp"
#include "frame.hpp"

#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
using sock_t = SOCKET;
static const sock_t kBad = INVALID_SOCKET;
static int close_sock(sock_t s) { return closesocket(s); }
static int last_error() { return WSAGetLastError(); }
#define poll WSAPoll
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
using sock_t = int;
static const sock_t kBad = -1;
static int close_sock(sock_t s) { return ::close(s); }
static int last_error() { return errno; }
#endif

namespace bridge {

struct Tunnel::Sock {
    sock_t   fd = kBad;
    uint32_t via = 0;      /* the multicast interface last set, so a send sets it only on a change */
};

static void nonblock(sock_t s) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static std::string why(const char* what) {
    return std::string(what) + " failed (" + std::to_string(last_error()) + ")";
}

Tunnel::Tunnel(Deliver deliver) : deliver_(std::move(deliver)), ifaces_(interfaces()) {
    /* a self connected loopback socket cuts the receive wait short when the socket set changes */
    sock_t w = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    if (w != kBad && ::bind(w, (sockaddr*)&a, sizeof a) == 0 && getsockname(w, (sockaddr*)&a, &len) == 0 &&
        ::connect(w, (sockaddr*)&a, sizeof a) == 0) {
        nonblock(w);
        waker_ = (intptr_t)w;
    } else if (w != kBad) close_sock(w);
    thread_ = std::thread([this] { run(); });
}

Tunnel::~Tunnel() {
    stop_ = true;
    wake();
    if (thread_.joinable()) thread_.join();
    for (auto& kv : socks_) close_sock(kv.second->fd);
    if (waker_ != -1) close_sock((sock_t)waker_);
}

void Tunnel::wake() {
    if (waker_ != -1) ::send((sock_t)waker_, "w", 1, 0);
}

uint16_t Tunnel::bind(uint16_t sock, uint32_t if_naddr, uint16_t port, bool reuse) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (!socks_.count(sock) && socks_.size() >= kMaxSockets) throw std::runtime_error("too many tunnel sockets");
    }
    sock_t fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == kBad) throw std::runtime_error(why("socket"));
    int on = 1;
    if (reuse) {
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
#ifdef SO_REUSEPORT
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char*)&on, sizeof on);
#endif
    }
    int buf = 4 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*)&buf, sizeof buf);
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&buf, sizeof buf);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = if_naddr;
    a.sin_port = htons(port);
    if (::bind(fd, (sockaddr*)&a, sizeof a) != 0) {
        std::string w = why("bind");
        close_sock(fd);
        throw std::runtime_error(w);
    }
    socklen_t len = sizeof a;
    getsockname(fd, (sockaddr*)&a, &len);
    nonblock(fd);
#ifdef _WIN32
    /* an ICMP port unreachable from a send must not fail the next receive */
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(fd, _WSAIOW(IOC_VENDOR, 12), &off, sizeof off, nullptr, 0, &ret, nullptr, nullptr);
#endif
    auto s = std::make_shared<Sock>();
    s->fd = fd;
    std::shared_ptr<Sock> old;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto& slot = socks_[sock];
        old = slot;
        slot = s;
    }
    if (old) close_sock(old->fd);
    wake();
    return ntohs(a.sin_port);
}

void Tunnel::membership(uint16_t sock, uint32_t group, uint32_t if_naddr, bool join) {
    std::shared_ptr<Sock> s;
    { std::lock_guard<std::mutex> g(mu_); auto it = socks_.find(sock); if (it != socks_.end()) s = it->second; }
    if (!s) throw std::runtime_error("no such tunnel socket");
    ip_mreq m{};
    m.imr_multiaddr.s_addr = group;
    m.imr_interface.s_addr = if_naddr;
    if (setsockopt(s->fd, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, (const char*)&m, sizeof m) != 0 && join)
        throw std::runtime_error(why("joining the group"));
}

void Tunnel::ttl(uint16_t sock, int ttl) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = socks_.find(sock);
    if (it == socks_.end()) return;
    int v = ttl;
    setsockopt(it->second->fd, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&v, sizeof v);
}

void Tunnel::loop(uint16_t sock, bool on) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = socks_.find(sock);
    if (it == socks_.end()) return;
    int v = on ? 1 : 0;
    setsockopt(it->second->fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&v, sizeof v);
}

void Tunnel::own_groups(uint16_t sock) {
#ifdef IP_MULTICAST_ALL
    std::lock_guard<std::mutex> g(mu_);
    auto it = socks_.find(sock);
    if (it == socks_.end()) return;
    int v = 0;
    setsockopt(it->second->fd, IPPROTO_IP, IP_MULTICAST_ALL, (const char*)&v, sizeof v);
#else
    (void)sock;
#endif
}

void Tunnel::close(uint16_t sock) {
    std::shared_ptr<Sock> s;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = socks_.find(sock);
        if (it == socks_.end()) return;
        s = it->second;
        socks_.erase(it);
    }
    close_sock(s->fd);
    wake();
}

/* A Rant group (239/8), loopback, or an address on one of this host's subnets. */
bool Tunnel::allowed(const uint8_t ip[4]) const {
    if (ip[0] == 239 || ip[0] == 127) return true;
    uint32_t a;
    std::memcpy(&a, ip, 4);
    for (const Iface& f : ifaces_)
        if (f.mask && (a & f.mask) == (f.addr & f.mask)) return true;
    return false;
}

bool Tunnel::send(uint16_t sock, const uint8_t ip[4], uint16_t port, uint32_t via, const uint8_t* data, size_t n) {
    if (!allowed(ip)) return false;
    std::shared_ptr<Sock> s;
    { std::lock_guard<std::mutex> g(mu_); auto it = socks_.find(sock); if (it != socks_.end()) s = it->second; }
    if (!s) return false;
    if (ip[0] >= 224 && ip[0] <= 239 && via && via != s->via) {
        in_addr i{};
        i.s_addr = via;
        setsockopt(s->fd, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&i, sizeof i);
        s->via = via;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    std::memcpy(&a.sin_addr.s_addr, ip, 4);
    a.sin_port = htons(port);
    ::sendto(s->fd, (const char*)data, (int)n, 0, (sockaddr*)&a, sizeof a);    /* best effort, as UDP */
    return true;
}

void Tunnel::run() {
    std::vector<uint8_t> buf(64 * 1024);
    std::string batch;
    auto flush = [&] {
        if (batch.size() > 2) deliver_(batch);
        batch.assign({ (char)OpUdp, 0 });
    };
    flush();
    std::vector<pollfd> fds;
    std::vector<std::pair<uint16_t, std::shared_ptr<Sock>>> order;
    while (!stop_) {
        fds.clear();
        order.clear();
        if (waker_ != -1) fds.push_back({ (sock_t)waker_, POLLIN, 0 });
        {
            std::lock_guard<std::mutex> g(mu_);
            for (auto& kv : socks_) { fds.push_back({ kv.second->fd, POLLIN, 0 }); order.emplace_back(kv.first, kv.second); }
        }
        int r = poll(fds.data(), (unsigned long)fds.size(), 1000);
        if (r <= 0) continue;
        size_t first = waker_ != -1 ? 1 : 0;
        if (first && (fds[0].revents & POLLIN)) {
            char c[16];
            while (::recv((sock_t)waker_, c, sizeof c, 0) > 0) {}
        }
        for (size_t i = first; i < fds.size(); i++) {
            if (!(fds[i].revents & POLLIN)) continue;
            auto& [id, s] = order[i - first];
            for (;;) {    /* drain what is queued, so one wake serves a burst in one batch */
                sockaddr_in from{};
                socklen_t len = sizeof from;
                int n = ::recvfrom(s->fd, (char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&from, &len);
                if (n < 0) break;
                if (batch.size() + kUdpRecord + (size_t)n > kUdpBatch) flush();
                uint16_t port = ntohs(from.sin_port);
                char rec[kUdpRecord] = {};
                rec[0] = (char)(id & 0xff); rec[1] = (char)(id >> 8);
                std::memcpy(rec + 2, &from.sin_addr.s_addr, 4);
                rec[6] = (char)(port & 0xff); rec[7] = (char)(port >> 8);
                rec[12] = (char)(n & 0xff); rec[13] = (char)(n >> 8);
                batch.append(rec, kUdpRecord);
                batch.append((const char*)buf.data(), (size_t)n);
            }
        }
        flush();
    }
}

std::vector<Tunnel::Iface> Tunnel::interfaces() {
    std::vector<Iface> out;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<uint8_t> mem(size);
    auto* list = (IP_ADAPTER_ADDRESSES*)mem.data();
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &size) == ERROR_BUFFER_OVERFLOW) {
        mem.resize(size);
        list = (IP_ADAPTER_ADDRESSES*)mem.data();
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &size) != NO_ERROR) return out;
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

}
