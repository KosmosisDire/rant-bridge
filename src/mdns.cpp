#include "mdns.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace bridge {

using clock_type = std::chrono::steady_clock;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace mdns {

static constexpr uint16_t kClassIn = 1;
static constexpr uint16_t kTopBit = 0x8000;    /* QU in a question, cache flush in a record */

bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

namespace {

struct Reader {
    const uint8_t* p;
    size_t n, at = 0;
    bool ok = true;

    uint8_t u8() { if (at + 1 > n) { ok = false; return 0; } return p[at++]; }
    uint16_t u16() { uint16_t hi = u8(); return (uint16_t)((hi << 8) | u8()); }
    uint32_t u32() { uint32_t hi = u16(); return (hi << 16) | u16(); }

    /* A name at the cursor, following compression pointers with a bound on the hops. */
    std::string name() {
        std::string out;
        size_t pos = at, hops = 0;
        bool jumped = false;
        for (;;) {
            if (pos >= n || hops > 32) { ok = false; return out; }
            uint8_t len = p[pos];
            if ((len & 0xc0) == 0xc0) {
                if (pos + 1 >= n) { ok = false; return out; }
                size_t to = (size_t)((len & 0x3f) << 8 | p[pos + 1]);
                if (!jumped) at = pos + 2;
                jumped = true;
                pos = to;
                hops++;
                continue;
            }
            if (len & 0xc0) { ok = false; return out; }
            pos++;
            if (len == 0) break;
            if (pos + len > n) { ok = false; return out; }
            if (!out.empty()) out += '.';
            out.append((const char*)p + pos, len);
            pos += len;
        }
        if (!jumped) at = pos;
        return out;
    }
};

bool record(Reader& r, Record& out) {
    out.name = r.name();
    out.type = r.u16();
    uint16_t cls = r.u16();
    out.flush = (cls & kTopBit) != 0;
    out.ttl = r.u32();
    uint16_t len = r.u16();
    if (!r.ok || r.at + len > r.n) return false;
    size_t end = r.at + len;
    switch (out.type) {
    case A:
        if (len != 4) return false;
        std::memcpy(&out.ip, r.p + r.at, 4);
        break;
    case PTR:
        out.target = r.name();
        break;
    case SRV:
        r.u16(); r.u16();               /* priority and weight */
        out.port = r.u16();
        out.target = r.name();
        break;
    case TXT:
        for (size_t at = r.at; at < end;) {
            size_t l = r.p[at];
            if (at + 1 + l > end) return false;
            out.txt.emplace_back((const char*)r.p + at + 1, l);
            at += 1 + l;
        }
        break;
    default:
        break;
    }
    r.at = end;
    return r.ok;
}

void put16(std::string& s, uint16_t v) { s += (char)(v >> 8); s += (char)(v & 0xff); }
void put32(std::string& s, uint32_t v) { put16(s, (uint16_t)(v >> 16)); put16(s, (uint16_t)(v & 0xffff)); }

void put_name(std::string& s, const std::string& name) {
    for (size_t at = 0; at < name.size();) {
        size_t dot = name.find('.', at);
        if (dot == std::string::npos) dot = name.size();
        size_t len = std::min<size_t>(dot - at, 63);
        s += (char)len;
        s.append(name, at, len);
        at = dot + 1;
    }
    s += '\0';
}

void put_record(std::string& s, const Record& r) {
    put_name(s, r.name);
    put16(s, r.type);
    put16(s, (uint16_t)(kClassIn | (r.flush ? kTopBit : 0)));
    put32(s, r.ttl);
    std::string data;
    switch (r.type) {
    case A: data.append((const char*)&r.ip, 4); break;
    case PTR: put_name(data, r.target); break;
    case SRV: put16(data, 0); put16(data, 0); put16(data, r.port); put_name(data, r.target); break;
    case TXT:
        for (const auto& t : r.txt) { data += (char)std::min<size_t>(t.size(), 255); data.append(t, 0, 255); }
        if (r.txt.empty()) data += '\0';
        break;
    default: break;
    }
    put16(s, (uint16_t)data.size());
    s += data;
}

}

bool parse(const uint8_t* p, size_t n, Message& out) {
    Reader r{ p, n };
    out.id = r.u16();
    uint16_t flags = r.u16();
    out.response = (flags & 0x8000) != 0;
    uint16_t qd = r.u16(), an = r.u16(), ns = r.u16(), ar = r.u16();
    if (!r.ok) return false;
    for (uint16_t i = 0; i < qd; i++) {
        Question q;
        q.name = r.name();
        q.type = r.u16();
        uint16_t cls = r.u16();
        q.unicast = (cls & kTopBit) != 0;
        if (!r.ok) return false;
        out.questions.push_back(q);
    }
    for (uint32_t i = 0; i < (uint32_t)an + ns + ar; i++) {
        Record rec;
        if (!record(r, rec)) return false;
        if (i < an) out.answers.push_back(rec);
        else if (i >= (uint32_t)an + ns) out.additional.push_back(rec);
    }
    return true;
}

std::string build(const Message& m) {
    std::string s;
    put16(s, m.id);
    put16(s, m.response ? 0x8400 : 0);      /* a response is authoritative */
    put16(s, (uint16_t)m.questions.size());
    put16(s, (uint16_t)m.answers.size());
    put16(s, 0);
    put16(s, (uint16_t)m.additional.size());
    for (const auto& q : m.questions) {
        put_name(s, q.name);
        put16(s, q.type);
        put16(s, (uint16_t)(kClassIn | (q.unicast ? kTopBit : 0)));
    }
    for (const auto& r : m.answers) put_record(s, r);
    for (const auto& r : m.additional) put_record(s, r);
    return s;
}

}

/* ---- the finder ----------------------------------------------------------------------- */

static constexpr uint16_t kPort = 5353;
static constexpr uint32_t kTtl = 120;
static const uint8_t kGroup[4] = { 224, 0, 0, 251 };

static uint32_t group_naddr() { uint32_t g; std::memcpy(&g, kGroup, 4); return g; }

static std::string dotted(uint32_t naddr) {
    const uint8_t* b = (const uint8_t*)&naddr;
    return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." + std::to_string(b[3]);
}

static std::string host_name() {
    char b[256] = {};
    if (gethostname(b, sizeof b - 1) != 0 || !b[0]) return "rant";
    std::string s(b);
    return s.substr(0, s.find('.'));    /* the host, not its domain */
}

/* A DNS label: letters, digits and hyphens, at most 63. */
static std::string label(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (std::isalnum((unsigned char)c)) out += (char)std::tolower((unsigned char)c);
        else if (!out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.substr(0, 63);
}

std::vector<Iface> advertised_interfaces(const std::string& bind) {
    uint32_t want = 0;
    inet_pton(AF_INET, bind.c_str(), &want);
    if (!want) return local_interfaces();
    const uint8_t* b = (const uint8_t*)&want;
    if (b[0] == 127) return { { want, 0xff } };     /* 255.0.0.0 */
    for (const Iface& f : local_interfaces())
        if (f.addr == want) return { f };
    return {};
}

Finder::Finder(std::vector<Iface> ifaces, uint16_t port, std::string version)
    : ifaces_(std::move(ifaces)), port_(port), version_(std::move(version)) {
    host_name_ = host_name();
    /* a second bridge on this host is told apart by its port */
    std::string inst = port_ == 7480 ? host_name_ : host_name_ + " (" + std::to_string(port_) + ")";
    instance_ = inst.substr(0, 63) + "." + mdns::kService;
    host_ = "rant-bridge-" + label(host_name_) + ".local";
    if (ifaces_.empty()) return;
    sock_t fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == kBadSock) return;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char*)&on, sizeof on);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(kPort);
    if (::bind(fd, (sockaddr*)&a, sizeof a) != 0) {
        std::fprintf(stderr, "rant-bridge: no mDNS, so it lists no other bridges: %s\n", failed("binding port 5353").c_str());
        close_sock(fd);
        return;
    }
    int ttl = 255, loop = 1;      /* mDNS asks for 255, and bridges on one host hear each other */
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof ttl);
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof loop);
    for (const Iface& f : ifaces_) {
        ip_mreq m{};
        m.imr_multiaddr.s_addr = group_naddr();
        m.imr_interface.s_addr = f.addr;
        setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&m, sizeof m);
    }
    nonblock(fd);
    fd_ = fd;
    thread_ = std::thread([this] { run(); });
}

Finder::~Finder() {
    stop_ = true;
    waker_.wake();
    if (thread_.joinable()) thread_.join();
    if (fd_ != kBadSock) {
        announce(0);     /* a goodbye, so the others drop this bridge at once */
        close_sock(fd_);
    }
}

/* Our address on the peer's subnet, or the first one. */
uint32_t Finder::local_for(uint32_t peer) const {
    for (const Iface& f : ifaces_)
        if (f.mask && (peer & f.mask) == (f.addr & f.mask)) return f.addr;
    return ifaces_.empty() ? 0 : ifaces_[0].addr;
}

/* This bridge's records at ip: the service pointer, its SRV and TXT, and the host's address. */
void Finder::add_own(mdns::Message& m, uint32_t ip, uint32_t ttl, bool ptr, bool srv_txt, bool host) const {
    if (ptr) {
        mdns::Record r; r.name = mdns::kService; r.type = mdns::PTR; r.ttl = ttl; r.target = instance_;
        m.answers.push_back(r);
    }
    if (srv_txt) {
        mdns::Record s; s.name = instance_; s.type = mdns::SRV; s.ttl = ttl; s.flush = true; s.port = port_; s.target = host_;
        mdns::Record t; t.name = instance_; t.type = mdns::TXT; t.ttl = ttl; t.flush = true; t.txt = { "v=" + version_, "name=" + host_name_ };
        (ptr ? m.additional : m.answers).push_back(s);
        (ptr ? m.additional : m.answers).push_back(t);
    }
    if (host) {
        mdns::Record r; r.name = host_; r.type = mdns::A; r.ttl = ttl; r.flush = true; r.ip = ip;
        (ptr || srv_txt ? m.additional : m.answers).push_back(r);
    }
}

void Finder::send(const mdns::Message& m, uint32_t to_ip, uint16_t to_port, uint32_t via) {
    if (fd_ == kBadSock) return;
    if (via) {
        in_addr i{};
        i.s_addr = via;
        setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&i, sizeof i);
    }
    std::string bytes = mdns::build(m);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = to_ip;
    a.sin_port = htons(to_port);
    ::sendto(fd_, bytes.data(), (int)bytes.size(), 0, (sockaddr*)&a, sizeof a);
}

void Finder::announce(uint32_t ttl) {
    for (const Iface& f : ifaces_) {
        mdns::Message m;
        m.response = true;
        add_own(m, f.addr, ttl, true, true, true);
        send(m, group_naddr(), kPort, f.addr);
    }
}

void Finder::browse() {
    for (const Iface& f : ifaces_) {
        mdns::Message q;
        q.questions.push_back({ mdns::kService, mdns::PTR, false });
        send(q, group_naddr(), kPort, f.addr);
    }
}

void Finder::on_query(const mdns::Message& q, uint32_t from_ip, uint16_t from_port) {
    uint32_t ip = local_for(from_ip);
    mdns::Message r;
    r.response = true;
    /* Only a plain resolver, asking from a port of its own, gets the answer alone. A responder
     * on 5353 gets it by multicast even when it asked otherwise, since a host shares that port
     * among several sockets and a unicast reaches only one of them. */
    bool unicast = from_port != kPort;
    for (const auto& question : q.questions) {
        bool any = question.type == mdns::ANY;
        if ((any || question.type == mdns::A) && mdns::same_name(question.name, mdns::kSharedName)) {
            /* shared: every bridge answers this name with its own address */
            mdns::Record a; a.name = mdns::kSharedName; a.type = mdns::A; a.ttl = unicast ? 10 : kTtl; a.ip = ip;
            r.answers.push_back(a);
        } else if ((any || question.type == mdns::A) && mdns::same_name(question.name, host_)) {
            add_own(r, ip, kTtl, false, false, true);
        } else if ((any || question.type == mdns::PTR) && mdns::same_name(question.name, mdns::kService)) {
            add_own(r, ip, kTtl, true, true, true);
        } else if ((any || question.type == mdns::SRV || question.type == mdns::TXT) && mdns::same_name(question.name, instance_)) {
            add_own(r, ip, kTtl, false, true, true);
        }
    }
    if (r.answers.empty()) return;
    if (unicast) {
        /* a plain resolver wants its id and question back */
        r.id = q.id;
        r.questions = q.questions;
        send(r, from_ip, from_port, ip);
    } else send(r, group_naddr(), kPort, ip);
}

void Finder::on_response(const mdns::Message& r, uint32_t from_ip) {
    auto now = clock_type::now();
    std::lock_guard<std::mutex> g(mu_);
    for (const auto* list : { &r.answers, &r.additional }) {
        for (const auto& rec : *list) {
            if (rec.type == mdns::PTR && mdns::same_name(rec.name, mdns::kService)) {
                if (mdns::same_name(rec.target, instance_)) continue;
                if (rec.ttl == 0) { peers_.erase(rec.target); continue; }
                Peer& p = peers_[rec.target];
                p.seen = now;
                p.from = from_ip;
            } else if (rec.type == mdns::SRV && !mdns::same_name(rec.name, instance_)) {
                Peer& p = peers_[rec.name];
                p.target = rec.target;
                p.port = rec.port;
                p.seen = now;
                p.from = from_ip;
            } else if (rec.type == mdns::TXT && !mdns::same_name(rec.name, instance_)) {
                Peer& p = peers_[rec.name];
                for (const auto& kv : rec.txt) {
                    if (kv.rfind("v=", 0) == 0) p.version = kv.substr(2);
                    else if (kv.rfind("name=", 0) == 0) p.name = kv.substr(5);
                }
            } else if (rec.type == mdns::A && !mdns::same_name(rec.name, host_)) {
                hosts_[rec.name] = rec.ip;
            }
        }
    }
}

std::vector<FoundBridge> Finder::bridges() {
    std::vector<FoundBridge> out;
    for (const Iface& f : ifaces_) out.push_back({ host_name_, dotted(f.addr), port_, version_, true });
    if (ifaces_.empty()) out.push_back({ host_name_, "127.0.0.1", port_, version_, true });
    auto now = clock_type::now();
    std::lock_guard<std::mutex> g(mu_);
    for (auto it = peers_.begin(); it != peers_.end();) {
        Peer& p = it->second;
        if (now - p.seen > seconds(3 * 60)) { it = peers_.erase(it); continue; }
        if (p.port) {
            auto h = hosts_.find(p.target);
            uint32_t ip = h != hosts_.end() ? h->second : p.from;
            std::string name = !p.name.empty() ? p.name : it->first.substr(0, it->first.find('.'));
            out.push_back({ name, dotted(ip), p.port, p.version, false });
        }
        ++it;
    }
    return out;
}

void Finder::run() {
    std::vector<uint8_t> buf(9000);
    /* announce twice and ask three times at the start, as RFC 6762 does, then ask each minute */
    auto start = clock_type::now();
    std::vector<milliseconds> asks = { milliseconds(0), milliseconds(1000), milliseconds(3000) };
    size_t asked = 0;
    int announced = 0;
    auto next_minute = start + seconds(60);
    while (!stop_) {
        auto now = clock_type::now();
        if (announced < 2 && now >= start + milliseconds(1000 * announced)) { announce(kTtl); announced++; }
        if (asked < asks.size() && now >= start + asks[asked]) { browse(); asked++; }
        if (now >= next_minute) { browse(); next_minute = now + seconds(60); }
        pollfd fds[2] = { { fd_, POLLIN, 0 }, { waker_.fd(), POLLIN, 0 } };
        int r = poll_socks(fds, waker_.ok() ? 2 : 1, 500);
        if (r <= 0) continue;
        if (waker_.ok() && (fds[1].revents & POLLIN)) waker_.drain();
        if (!(fds[0].revents & POLLIN)) continue;
        for (;;) {
            sockaddr_in from{};
            socklen_t len = sizeof from;
            int n = ::recvfrom(fd_, (char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&from, &len);
            if (n <= 0) break;
            mdns::Message m;
            if (!mdns::parse(buf.data(), (size_t)n, m)) continue;
            if (m.response) on_response(m, from.sin_addr.s_addr);
            else on_query(m, from.sin_addr.s_addr, ntohs(from.sin_port));
        }
    }
}

}
