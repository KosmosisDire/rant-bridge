#include "session.hpp"

#include <ixwebsocket/IXWebSocket.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace bridge {

using json = nlohmann::json;
using clock_type = std::chrono::steady_clock;
using std::chrono::duration_cast;
using std::chrono::microseconds;
using std::chrono::milliseconds;

static void put32(std::string& s, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) s[at + i] = (char)(uint8_t)(v >> (8 * i));
}

static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static std::string dotted(uint32_t naddr) {
    const uint8_t* b = (const uint8_t*)&naddr;
    return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." + std::to_string(b[3]);
}

static uint32_t naddr_of(const std::string& dotted_ip) {
    uint8_t b[4] = { 0, 0, 0, 0 };
    unsigned v[4] = { 0, 0, 0, 0 };
    if (std::sscanf(dotted_ip.c_str(), "%u.%u.%u.%u", &v[0], &v[1], &v[2], &v[3]) == 4)
        for (int i = 0; i < 4; i++) b[i] = (uint8_t)v[i];
    uint32_t n;
    std::memcpy(&n, b, 4);
    return n;
}

Session::Session(const Config& cfg, std::string token) : cfg_(cfg), token_(std::move(token)) {
    ticker_ = std::thread([this] { run(); });
}

Session::~Session() {
    {
        std::lock_guard<std::mutex> g(mu_);
        stop_ = true;
        attached_ = false;
    }
    cv_.notify_all();
    if (ticker_.joinable()) ticker_.join();
    { std::lock_guard<std::recursive_mutex> g(ws_mu_); ws_ = nullptr; }
    tunnel_.reset();    /* its thread delivers into this session, so it stops first */
    std::shared_ptr<Rtc> rtc;
    { std::lock_guard<std::mutex> g(mu_); rtc = std::move(rtc_); }
}

json Session::open(const json& req) {
    {
        std::lock_guard<std::mutex> g(mu_);
        probe_ms_ = std::max(10u, req.value("probe_interval_ms", 250u));
    }
    name_ = req.value("name", std::string("tunnel"));
    tunnel_ = std::make_unique<Tunnel>([this](const std::string& batch) { to_client(batch); });
    return reopen_reply();
}

json Session::reopen_reply() const {
    json ifs = json::array();
    for (const Iface& f : local_interfaces()) ifs.push_back({ {"addr", dotted(f.addr)}, {"mask", dotted(f.mask)} });
    /* a POSIX host hands group traffic only to a socket bound to any address */
#ifdef _WIN32
    const bool group_binds_any = false;
#else
    const bool group_binds_any = true;
#endif
    return { {"proto", kProtocol}, {"name", name_}, {"webrtc", cfg_.webrtc}, {"interfaces", ifs}, {"group_binds_any", group_binds_any} };
}

void Session::attach(ix::WebSocket* ws) {
    std::shared_ptr<Rtc> old;     /* a returning client negotiates WebRTC afresh */
    { std::lock_guard<std::recursive_mutex> g(ws_mu_); ws_ = ws; }
    std::lock_guard<std::mutex> g(mu_);
    attached_ = true;
    detached_at_ = {};
    old = std::move(rtc_);
    link_.out.clear();
    link_.last_heard = clock_type::now();
    link_.missed = 0;
}

void Session::detach(ix::WebSocket* ws) {
    std::shared_ptr<Rtc> rtc;
    {
        std::lock_guard<std::recursive_mutex> g(ws_mu_);
        if (ws_ != ws) return;
        ws_ = nullptr;
    }
    std::lock_guard<std::mutex> g(mu_);
    attached_ = false;
    detached_at_ = clock_type::now();
    rtc = std::move(rtc_);
}

milliseconds Session::detached_for() const {
    std::lock_guard<std::mutex> g(mu_);
    if (attached_ || detached_at_ == clock_type::time_point{}) return milliseconds(0);
    return duration_cast<milliseconds>(clock_type::now() - detached_at_);
}

/* ---- sending ------------------------------------------------------------------------ */

void Session::send_json(const json& j) {
    std::string text = j.dump(-1, ' ', false, json::error_handler_t::replace);
    std::lock_guard<std::recursive_mutex> g(ws_mu_);
    if (ws_) ws_->send(text, false);
}

void Session::reply(const json& seq, json extra) {
    extra["op"] = "reply"; extra["seq"] = seq; extra["ok"] = true;
    send_json(extra);
}

void Session::reply_error(const json& seq, const std::string& why) {
    send_json({ {"op", "reply"}, {"seq", seq}, {"ok", false}, {"error", why} });
}

void Session::send_ping(const std::string& ping) {
    std::shared_ptr<Rtc> rtc;
    { std::lock_guard<std::mutex> g(mu_); rtc = rtc_; }
    if (rtc && rtc->up() && rtc->send(kPingChannel, ping)) return;
    std::lock_guard<std::recursive_mutex> g(ws_mu_);
    if (ws_) ws_->sendBinary(ping);
}

/* A batch for the client's node, on the tunnel channel while WebRTC is up, else the WebSocket.
 * One carrier only, so datagrams keep their order, and dropped past a socket's worth of
 * backlog, as UDP would drop them. */
void Session::to_client(const std::string& batch) {
    std::shared_ptr<Rtc> rtc;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (!attached_) return;
        rtc = rtc_;
    }
    if (rtc && rtc->up() && rtc->has(kTunnelChannel)) { rtc->send(kTunnelChannel, batch, kTunnelBacklog); return; }
    std::lock_guard<std::recursive_mutex> g(ws_mu_);
    if (ws_ && ws_->bufferedAmount() < kTunnelBacklog) ws_->sendBinary(batch);
}

/* ---- liveness ----------------------------------------------------------------------- */

void Session::run() {
    std::unique_lock<std::mutex> lk(mu_);
    while (!stop_) {
        cv_.wait_for(lk, milliseconds(std::min<uint32_t>(probe_ms_, 50)), [this] { return stop_; });
        if (stop_) break;
        lk.unlock();
        tick();
        lk.lock();
    }
}

void Session::tick() {
    auto now = clock_type::now();
    std::string ping;
    bool drop = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (!attached_) return;
        if (now - link_.last_ping >= milliseconds(probe_ms_)) {
            link_.last_ping = now;
            uint32_t seq = ++link_.next_seq;
            link_.out[seq] = now;
            while (link_.out.size() > 64) link_.out.erase(link_.out.begin());
            ping.assign(kPingBytes, '\0');
            ping[0] = (char)kPingOp;
            put32(ping, 1, seq);
            put32(ping, 5, link_.last_us);
            put32(ping, 9, link_.jitter_us);
            put32(ping, 13, link_.samples);
            put32(ping, 17, link_.missed);
        }
        auto silent = duration_cast<milliseconds>(now - link_.last_heard).count();
        long long half = probe_ms_ / 2;
        link_.missed = (uint32_t)(silent > half ? (silent - half) / probe_ms_ : 0);
        drop = silent > (long long)cfg_.detach_ms;
    }
    if (!ping.empty()) send_ping(ping);
    if (drop) {
        std::lock_guard<std::recursive_mutex> g(ws_mu_);
        if (ws_) {
            std::fprintf(stderr, "rant-bridge: %s stopped answering pings, closing it\n", name_.c_str());
            ws_->close(4008, "no answer to pings");
        }
    }
}

/* Any message from the client counts as heard. */
void Session::heard() {
    std::lock_guard<std::mutex> g(mu_);
    link_.last_heard = clock_type::now();
}

void Session::on_pong(const uint8_t* p, size_t n) {
    if (n < kPongBytes) return;
    std::lock_guard<std::mutex> g(mu_);
    auto it = link_.out.find(get32(p + 1));
    if (it == link_.out.end()) return;
    auto now = clock_type::now();
    uint32_t rtt = (uint32_t)duration_cast<microseconds>(now - it->second).count();
    link_.out.erase(link_.out.begin(), std::next(it));
    /* RFC 6298 smoothing for the jitter, the latest sample as the reading */
    if (!link_.samples) { link_.srtt_us = rtt; link_.jitter_us = rtt / 2; }
    else {
        uint32_t diff = link_.srtt_us > rtt ? link_.srtt_us - rtt : rtt - link_.srtt_us;
        link_.jitter_us = (3 * link_.jitter_us + diff) / 4;
        link_.srtt_us = (7 * link_.srtt_us + rtt) / 8;
    }
    link_.last_us = rtt;
    link_.samples++;
}

/* ---- from the client ---------------------------------------------------------------- */

void Session::on_text(const json& req) {
    heard();
    json seq = req.value("seq", json(0));
    std::string op = req.value("op", "");
    try {
        if (op == "rtc") op_rtc(req, seq);
        else if (op.rfind("udp_", 0) == 0) op_udp(op, req, seq);
        else reply_error(seq, "unknown op '" + op + "'");
    } catch (const std::exception& e) {
        reply_error(seq, e.what());
    }
}

void Session::on_binary(const uint8_t* p, size_t n) {
    heard();
    if (!n) return;
    if (p[0] == kUdpOp) on_udp(p, n);
    else if (p[0] == kPongOp) on_pong(p, n);
}

void Session::op_rtc(const json& req, const json& seq) {
    if (!cfg_.webrtc) { reply_error(seq, "webrtc is off on this bridge"); return; }
    std::shared_ptr<Rtc> rtc;
    { std::lock_guard<std::mutex> g(mu_); rtc = rtc_; }
    if (req.contains("sdp")) {
        if (!rtc) { reply_error(seq, "start webrtc first"); return; }
        rtc->offer(req.value("sdp", ""), [this, seq](json answer) { reply(seq, std::move(answer)); });
        return;
    }
    if (req.contains("candidate")) {
        if (rtc) rtc->candidate(req.value("candidate", ""), req.value("mid", ""));
        reply(seq, {});
        return;
    }
    Rtc::Hooks h;
    h.push = [this](const json& j) { send_json(j); };
    h.message = [this](const uint8_t* p, size_t n) { on_binary(p, n); };
    rtc = std::make_shared<Rtc>(cfg_, h);
    json r = rtc->start();
    { std::lock_guard<std::mutex> g(mu_); rtc_ = rtc; }
    reply(seq, r);
}

void Session::op_udp(const std::string& op, const json& req, const json& seq) {
    uint16_t sock = (uint16_t)req.value("sock", 0);
    if (op == "udp_bind") {
        uint16_t port = tunnel_->bind(sock, naddr_of(req.value("if", std::string("0.0.0.0"))),
                                      (uint16_t)req.value("port", 0), req.value("reuse", false));
        reply(seq, { {"port", port} });
    } else if (op == "udp_member") {
        tunnel_->membership(sock, naddr_of(req.value("group", std::string())), naddr_of(req.value("if", std::string("0.0.0.0"))),
                            req.value("join", true));
        reply(seq, {});
    } else if (op == "udp_option") {
        if (req.contains("ttl")) tunnel_->ttl(sock, req.value("ttl", 1));
        if (req.contains("loop")) tunnel_->loop(sock, req.value("loop", true));
        reply(seq, {});
    } else if (op == "udp_close") {
        tunnel_->close(sock);
        reply(seq, {});
    } else reply_error(seq, "unknown op '" + op + "'");
}

/* A batch of datagrams from the client's node, each out of this host's socket. */
void Session::on_udp(const uint8_t* p, size_t n) {
    for (size_t at = 2; at + kUdpRecord <= n;) {
        const uint8_t* r = p + at;
        uint16_t sock = (uint16_t)(r[0] | (r[1] << 8));
        uint16_t port = (uint16_t)(r[6] | (r[7] << 8));
        uint32_t via;
        std::memcpy(&via, r + 8, 4);
        size_t len = (size_t)(r[12] | (r[13] << 8));
        if (at + kUdpRecord + len > n) return;
        tunnel_->send(sock, r + 2, port, via, r + kUdpRecord, len);
        at += kUdpRecord + len;
    }
}

}
