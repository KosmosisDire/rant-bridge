#pragma once

/* One client's node on the mesh and everything it created. A session outlives a dropped
 * connection for the grace period, so a client that comes back resumes where it was. */

#include "codec.hpp"
#include "config.hpp"
#include "frame.hpp"
#include "rtc.hpp"
#include "sender.hpp"

#include "rant.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace ix { class WebSocket; }

namespace bridge {

constexpr int kProtocol = 12;

struct Entity {
    enum Kind { Pub, Sub, FnDef, FnRemote, TaskDef, TaskRemote, VarDef, VarRemote } kind = Pub;
    uint16_t    id = 0;
    std::string name;
    bool        reliable = true;
    std::optional<rant::Publisher<rant::Bytes>> pub;
    std::optional<rant::Subscriber<rant::Bytes>> sub;
    std::optional<rant::FunctionDefinition<rant::Bytes, rant::Bytes>> fndef;
    std::optional<rant::RemoteFunction<rant::Bytes, rant::Bytes>> fnrem;
    std::optional<rant::TaskDefinition<rant::Bytes, rant::Bytes, rant::Bytes>> taskdef;
    std::optional<rant::RemoteTask<rant::Bytes, rant::Bytes, rant::Bytes>> taskrem;
    std::optional<rant::VariableDefinition<rant::Bytes>> vardef;
    std::optional<rant::RemoteVariable<rant::Bytes>> varrem;
    Codec value, req, prg, rsp;
    int last_match = -1;           /* packed count*2 + ready, the last one pushed */
    bool delayed = false;          /* a queue_delay warning is out for it */

    rant::VariableDefinition<rant::Bytes>* var() { return vardef ? &*vardef : varrem ? &*varrem : nullptr; }
};

class Session {
public:
    Session(const Config& cfg, std::string token);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /* Opens the node from an open request. Throws std::runtime_error with the reason. */
    nlohmann::json open(const nlohmann::json& req);
    /* A connection now speaks for this session; the previous one, if any, is gone. */
    void attach(ix::WebSocket* ws);
    /* That connection dropped. A newer connection that already took over is left alone. */
    void detach(ix::WebSocket* ws);
    /* The open reply for a client that resumed this session. */
    nlohmann::json reopen_reply() const;
    /* How long the session has had no connection, zero while attached. */
    std::chrono::milliseconds detached_for() const;
    /* A reliable lane overflowed while no client was attached: nothing can drain it. */
    bool overflowed() const { return overflowed_; }

    void on_text(const nlohmann::json& req);
    void on_binary(const uint8_t* p, size_t n);

    const std::string& token() const { return token_; }
    void set_token(std::string t) { token_ = std::move(t); }

private:
    struct ParkedTask {
        uint16_t entity = 0;
        rant::PendingTask<rant::Bytes, rant::Bytes> pt;
        bool cancel_sent = false;
    };
    struct Link {      /* the round trip to the client, from pings */
        uint32_t next_seq = 0;
        std::map<uint32_t, std::chrono::steady_clock::time_point> out;
        uint32_t last_us = 0, srtt_us = 0, jitter_us = 0, samples = 0, missed = 0;
        uint64_t at_us = 0;
        std::chrono::steady_clock::time_point last_pong{}, last_ping{};   /* last_pong: last heard at all */
    };

    void send_json(const nlohmann::json& j);
    void reply(const nlohmann::json& seq, nlohmann::json extra);
    void reply_error(const nlohmann::json& seq, const std::string& why);
    void event(nlohmann::json e);
    bool carrier(uint16_t id, const std::string& frame);
    void tick();
    void deliver(Entity& e, const Codec& codec, Outgoing m, rant::Bytes data, bool video);
    std::shared_ptr<Entity> find(uint16_t id);

    void op_create(const nlohmann::json& req, const nlohmann::json& seq);
    void op_close(const nlohmann::json& req, const nlohmann::json& seq);
    void op_refresh(const nlohmann::json& req, const nlohmann::json& seq);
    void op_peers(const nlohmann::json& seq);
    void op_reflect(const std::string& op, const nlohmann::json& req, const nlohmann::json& seq);
    void op_meta(const nlohmann::json& req, const nlohmann::json& seq);
    void op_stats(const nlohmann::json& seq);
    void op_rtc(const nlohmann::json& req, const nlohmann::json& seq);
    void op_cancel(const nlohmann::json& req, const nlohmann::json& seq);
    void op_log_subscribe(const nlohmann::json& req, const nlohmann::json& seq);
    nlohmann::json schemas(Entity& e, bool reflect);

    void on_call(Entity& e, const Frame& f, rant::Bytes payload);
    void on_result(const Frame& f, rant::Bytes payload);
    void on_progress(const Frame& f, rant::Bytes payload);
    void on_pong(const Frame& f);
    struct Busy;
    void heard();

    const Config& cfg_;
    std::string token_;
    std::optional<rant::Node> node_;
    std::string name_;
    uint32_t probe_ms_ = 250;

    mutable std::mutex mu_;      /* leaf lock: never call into Rant while holding it */
    bool attached_ = false;
    /* the socket has its own lock: a slow write must never hold the session state */
    std::mutex ws_mu_;
    ix::WebSocket* ws_ = nullptr;
    std::chrono::steady_clock::time_point detached_at_{};
    std::shared_ptr<Rtc> rtc_;
    std::map<uint16_t, std::shared_ptr<Entity>> ents_;
    std::map<uint32_t, rant::Deferred<rant::Bytes>> parked_;              /* function requests by request id */
    std::map<uint32_t, std::shared_ptr<ParkedTask>> parked_tasks_;        /* task requests by request id */
    std::map<uint32_t, std::pair<uint16_t, uint32_t>> task_calls_;        /* client call id to (entity, Rant call id) */
    uint32_t next_req_ = 0;
    uint8_t log_levels_ = 0;
    bool log_hooked_ = false;
    Link link_;
    Reassembly reasm_;
    std::atomic<bool> overflowed_{ false };
    std::atomic<int> busy_{ 0 };     /* a client message is being handled: its pongs wait behind it */
    std::unique_ptr<Sender> sender_;   /* last member: its thread stops first */
};

}
