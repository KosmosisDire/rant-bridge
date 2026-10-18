#include "tunnel.hpp"

#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <mswsock.h>
#else
#include <unistd.h>
#endif

namespace bridge {

struct Tunnel::Sock {
    sock_t   fd = kBadSock;
    uint32_t via = 0;      /* the multicast interface last set, so a send sets it only on a change */
};

Tunnel::Tunnel(Deliver deliver) : deliver_(std::move(deliver)), ifaces_(local_interfaces()) {
    thread_ = std::thread([this] { run(); });
}

Tunnel::~Tunnel() {
    stop_ = true;
    wake();
    if (thread_.joinable()) thread_.join();
    for (auto& kv : socks_) close_sock(kv.second->fd);
}

void Tunnel::wake() { waker_.wake(); }

uint16_t Tunnel::bind(uint16_t sock, uint32_t if_naddr, uint16_t port, bool reuse) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (!socks_.count(sock) && socks_.size() >= kMaxSockets) throw std::runtime_error("too many tunnel sockets");
    }
    sock_t fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == kBadSock) throw std::runtime_error(failed("socket"));
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
        std::string w = failed("bind");
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
#ifdef IP_MULTICAST_ALL
    if (join) {     /* Linux otherwise hands the socket every group any socket here joined */
        int off = 0;
        setsockopt(s->fd, IPPROTO_IP, IP_MULTICAST_ALL, (const char*)&off, sizeof off);
    }
#endif
    if (setsockopt(s->fd, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, (const char*)&m, sizeof m) != 0 && join)
        throw std::runtime_error(failed("joining the group"));
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
        batch.assign({ (char)kUdpOp, 0 });
    };
    flush();
    std::vector<pollfd> fds;
    std::vector<std::pair<uint16_t, std::shared_ptr<Sock>>> order;
    while (!stop_) {
        fds.clear();
        order.clear();
        if (waker_.ok()) fds.push_back({ waker_.fd(), POLLIN, 0 });
        {
            std::lock_guard<std::mutex> g(mu_);
            for (auto& kv : socks_) { fds.push_back({ kv.second->fd, POLLIN, 0 }); order.emplace_back(kv.first, kv.second); }
        }
        int r = poll_socks(fds.data(), fds.size(), 1000);
        if (r <= 0) continue;
        size_t first = waker_.ok() ? 1 : 0;
        if (first && (fds[0].revents & POLLIN)) waker_.drain();
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

}
