#pragma once

/* One client's tunnel: UDP sockets on this host for the Rant node the client runs itself. A
 * session outlives a dropped connection for the grace period, so a client that comes back
 * resumes with its sockets. Rant's spec/tunnel.md is the protocol. */

#include "config.hpp"
#include "rtc.hpp"
#include "tunnel.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ix { class WebSocket; }

namespace bridge {

constexpr int kProtocol = 13;

/* PING [u8 7][u32 seq][u32 us][u32 jitter_us][u32 samples][u32 missed] tells the client what
 * the bridge measures of its link, and PONG [u8 8][u32 seq] answers it. */
constexpr uint8_t kPingOp = 7;
constexpr uint8_t kPongOp = 8;
constexpr size_t  kPingBytes = 21;
constexpr size_t  kPongBytes = 5;
constexpr size_t  kTunnelBacklog = 4u << 20;   /* what may wait toward a client, as a socket buffer */

class Session {
public:
    Session(const Config& cfg, std::string token);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /* Sets the session up from an open request and returns the reply's fields. */
    nlohmann::json open(const nlohmann::json& req);
    /* A connection now speaks for this session, the previous one, if any, is gone. */
    void attach(ix::WebSocket* ws);
    /* That connection dropped. A newer connection that already took over is left alone. */
    void detach(ix::WebSocket* ws);
    /* The open reply for a client that resumed this session. */
    nlohmann::json reopen_reply() const;
    /* How long the session has had no connection, zero while attached. */
    std::chrono::milliseconds detached_for() const;

    void on_text(const nlohmann::json& req);
    void on_binary(const uint8_t* p, size_t n);

    const std::string& token() const { return token_; }
    void set_token(std::string t) { token_ = std::move(t); }

private:
    struct Link {      /* the round trip to the client, from pings */
        uint32_t next_seq = 0;
        std::map<uint32_t, std::chrono::steady_clock::time_point> out;
        uint32_t last_us = 0, srtt_us = 0, jitter_us = 0, samples = 0, missed = 0;
        std::chrono::steady_clock::time_point last_heard{}, last_ping{};
    };

    void send_json(const nlohmann::json& j);
    void reply(const nlohmann::json& seq, nlohmann::json extra);
    void reply_error(const nlohmann::json& seq, const std::string& why);
    /* A PING to the client, or to the anchor channel while WebRTC is up. */
    void send_ping(const std::string& ping);
    void run();
    void tick();
    void heard();

    void op_rtc(const nlohmann::json& req, const nlohmann::json& seq);
    void op_udp(const std::string& op, const nlohmann::json& req, const nlohmann::json& seq);
    void on_udp(const uint8_t* p, size_t n);
    void to_client(const std::string& batch);
    void on_pong(const uint8_t* p, size_t n);

    const Config& cfg_;
    std::string token_;
    std::string name_ = "tunnel";
    uint32_t probe_ms_ = 250;
    std::unique_ptr<Tunnel> tunnel_;

    mutable std::mutex mu_;      /* leaf lock */
    bool attached_ = false;
    /* the socket has its own lock: a slow write must never hold the session state. Recursive,
       since a send that finds the peer gone runs the close callback, which detaches, inside it */
    std::recursive_mutex ws_mu_;
    ix::WebSocket* ws_ = nullptr;
    std::chrono::steady_clock::time_point detached_at_{};
    std::shared_ptr<Rtc> rtc_;
    Link link_;

    /* the ticker: pings at the probe interval and the silence that detaches a client */
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread ticker_;
};

}
