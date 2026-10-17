#include "server.hpp"

#include "session.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

namespace bridge {

using json = nlohmann::json;
using clock_type = std::chrono::steady_clock;

namespace {

/* One WebSocket connection: before open it has no session, after it speaks for one. */
struct Conn {
    ix::WebSocket* ws = nullptr;
    std::shared_ptr<Session> session;
};

std::string new_token() {
    std::random_device rd;
    char b[33];
    for (int i = 0; i < 32; i++) b[i] = "0123456789abcdef"[rd() % 16];
    b[32] = 0;
    return b;
}

/* constant time, so the passphrase cannot be found one character at a time */
bool same(const std::string& a, const std::string& b) {
    unsigned diff = (unsigned)(a.size() ^ b.size());
    for (size_t i = 0; i < a.size() && i < b.size(); i++) diff |= (unsigned)(a[i] ^ b[i]);
    return diff == 0;
}

class Server {
public:
    explicit Server(const Config& cfg) : cfg_(cfg) {}

    void on_message(const std::shared_ptr<ix::ConnectionState>& state, ix::WebSocket& ws, const ix::WebSocketMessagePtr& msg) {
        const std::string id = state->getId();
        switch (msg->type) {
        case ix::WebSocketMessageType::Open: {
            auto o = msg->openInfo.headers.find("Origin");
            if (o != msg->openInfo.headers.end() && !cfg_.allowed_origins.empty()) {
                bool allowed = false;
                for (const auto& a : cfg_.allowed_origins) if (a == o->second) allowed = true;
                if (!allowed) { ws.close(4003, "origin not allowed"); return; }
            }
            std::lock_guard<std::mutex> g(mu_);
            auto c = std::make_shared<Conn>();
            c->ws = &ws;
            conns_[id] = c;
            return;
        }
        case ix::WebSocketMessageType::Message: {
            std::shared_ptr<Conn> c;
            { std::lock_guard<std::mutex> g(mu_); auto it = conns_.find(id); if (it != conns_.end()) c = it->second; }
            if (!c) return;
            if (msg->str.size() > cfg_.max_message_bytes) { ws.close(1009, "message too big"); return; }
            if (msg->binary) { if (c->session) c->session->on_binary((const uint8_t*)msg->str.data(), msg->str.size()); return; }
            json req = json::parse(msg->str, nullptr, false);
            if (req.is_discarded() || !req.is_object()) {
                ws.send(json({ {"op", "reply"}, {"seq", 0}, {"ok", false}, {"error", "malformed JSON"} }).dump());
                return;
            }
            if (c->session) {
                if (req.value("op", "") == "open") ws.send(json({ {"op", "reply"}, {"seq", req.value("seq", json(0))}, {"ok", false}, {"error", "already open"} }).dump());
                else c->session->on_text(req);
                return;
            }
            open(*c, req);
            return;
        }
        case ix::WebSocketMessageType::Close:
        case ix::WebSocketMessageType::Error: {
            std::shared_ptr<Conn> c;
            {
                std::lock_guard<std::mutex> g(mu_);
                auto it = conns_.find(id);
                if (it == conns_.end()) return;
                c = it->second;
                conns_.erase(it);
            }
            if (c->session) c->session->detach(c->ws);    /* kept for the grace period */
            return;
        }
        default:
            return;
        }
    }

    /* Ends sessions whose client did not come back in time. */
    void reap() {
        std::vector<std::shared_ptr<Session>> dead;
        {
            std::lock_guard<std::mutex> g(mu_);
            for (auto it = sessions_.begin(); it != sessions_.end();) {
                if (it->second->detached_for() > std::chrono::milliseconds(cfg_.grace_ms)) { dead.push_back(it->second); it = sessions_.erase(it); }
                else ++it;
            }
        }
        dead.clear();      /* each session closes its sockets here, outside the lock */
    }

    void close_all() {
        std::map<std::string, std::shared_ptr<Session>> all;
        { std::lock_guard<std::mutex> g(mu_); all.swap(sessions_); }
    }

private:
    void open(Conn& c, const json& req) {
        json seq = req.value("seq", json(0));
        auto refuse = [&](const std::string& why, bool hangup) {
            c.ws->send(json({ {"op", "reply"}, {"seq", seq}, {"ok", false}, {"error", why} }).dump());
            if (hangup) c.ws->close(4001, why);
        };
        if (req.value("op", "") != "open") { refuse("send open first", false); return; }
        if (req.value("proto", 0) != kProtocol) {
            refuse("this bridge speaks protocol " + std::to_string(kProtocol) + ", the client " + std::to_string(req.value("proto", 0)), true);
            return;
        }
        if (!cfg_.passphrase.empty() && !same(req.value("passphrase", std::string()), cfg_.passphrase)) {
            /* a slow refusal, not a lockout: behind a tunnel every client shares one address */
            std::this_thread::sleep_for(std::chrono::seconds(1));
            refuse("wrong passphrase", true);
            return;
        }

        std::shared_ptr<Session> s;
        json reply;
        std::string token = req.value("resume", std::string());
        if (!token.empty()) {
            std::lock_guard<std::mutex> g(mu_);
            auto it = sessions_.find(token);
            if (it != sessions_.end()) {
                s = it->second;
                sessions_.erase(it);
            }
        }
        bool resumed = s != nullptr;
        if (!s) {
            s = std::make_shared<Session>(cfg_, std::string());
            try { reply = s->open(req); }
            catch (const std::exception& e) { refuse(e.what(), false); return; }
        } else reply = s->reopen_reply();
        std::string fresh = new_token();
        s->set_token(fresh);
        {
            std::lock_guard<std::mutex> g(mu_);
            sessions_[fresh] = s;
            c.session = s;
        }
        reply["op"] = "reply"; reply["seq"] = seq; reply["ok"] = true;
        reply["resumed"] = resumed; reply["resume_token"] = fresh; reply["grace_ms"] = cfg_.grace_ms;
        c.ws->send(reply.dump());
        s->attach(c.ws);
    }

    const Config& cfg_;
    std::mutex mu_;
    std::map<std::string, std::shared_ptr<Conn>> conns_;
    std::map<std::string, std::shared_ptr<Session>> sessions_;   /* by resume token */
};

}

int serve(const Config& cfg, const std::function<bool()>& stop, const std::function<void(const std::string&)>& listening) {
    ix::initNetSystem();
    rtc::InitLogger(rtc::LogLevel::Warning, [](rtc::LogLevel, std::string m) { std::fprintf(stderr, "[webrtc] %s\n", m.c_str()); });
    rtc::Preload();
    Server srv(cfg);
    ix::WebSocketServer ws(cfg.port, cfg.bind, ix::SocketServer::kDefaultTcpBacklog, (size_t)cfg.max_connections);
    /* IXWebSocket compresses every message both ways when the client offers it, as browsers do:
       CPU spent on frames that are mostly binary values, video and datagrams */
    ws.disablePerMessageDeflate();
    ws.setOnClientMessageCallback([&srv](std::shared_ptr<ix::ConnectionState> st, ix::WebSocket& w, const ix::WebSocketMessagePtr& m) {
        try { srv.on_message(st, w, m); }
        catch (const std::exception& e) { std::fprintf(stderr, "rant-bridge: %s\n", e.what()); }
    });
    auto res = ws.listen();
    if (!res.first) {
        std::fprintf(stderr, "rant-bridge: cannot listen on %s:%u: %s\n", cfg.bind.c_str(), cfg.port, res.second.c_str());
        return 1;
    }
    ws.start();
    std::string host = cfg.bind == "0.0.0.0" ? "127.0.0.1" : cfg.bind;
    std::string url = "ws://" + host + ":" + std::to_string(cfg.port);
    std::fprintf(stderr, "rant-bridge listening on %s\n", url.c_str());
    if (listening) listening(url);
    while (!stop()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        srv.reap();
    }
    ws.stop();
    srv.close_all();
    rtc::Cleanup();
    ix::uninitNetSystem();
    return 0;
}

}
