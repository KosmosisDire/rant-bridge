#include "session.hpp"

#include <ixwebsocket/IXWebSocket.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>

namespace bridge {

using json = nlohmann::json;
using clock_type = std::chrono::steady_clock;
using FT = rant::FieldType;
using std::chrono::milliseconds;
using std::chrono::microseconds;

static const size_t kChunk = 16u << 10;
static const size_t kBestEffortCap = 1u << 20;      /* Rant's default queue_bytes */
static const size_t kReliableCap = 8u << 20;        /* past this a client cannot keep up */

static uint64_t now_us() {
    return (uint64_t)std::chrono::duration_cast<microseconds>(clock_type::now().time_since_epoch()).count();
}

static std::string hex64(uint64_t v) {
    char b[17];
    std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}

static const char* kind_str(FT k) {
    switch (k) {
    case FT::U8: return "u8"; case FT::U16: return "u16"; case FT::U32: return "u32"; case FT::U64: return "u64";
    case FT::I8: return "i8"; case FT::I16: return "i16"; case FT::I32: return "i32"; case FT::I64: return "i64";
    case FT::F32: return "f32"; case FT::F64: return "f64"; case FT::Bool: return "bool";
    case FT::Array: return "arr"; case FT::Struct: return "struct"; case FT::String: return "string";
    case FT::VString: return "vstring"; case FT::VArray: return "varr"; case FT::Map: return "map";
    case FT::Enum: return "enum"; default: return "?";
    }
}

/* One schema as the create reply and reflection carry it: enough to read, render and find media. */
static json schema_block(const Codec& c) {
    if (!c.typed()) return nullptr;
    json fields = json::array();
    const auto& rows = c.rows();
    for (size_t i = 0; i < rows.size(); i++) {
        const auto& f = rows[i];
        json r = { {"path", c.path((int)i)}, {"name", std::string(f.name)}, {"kind", kind_str(f.kind)} };
        if (!f.type_name.empty()) r["named"] = std::string(f.type_name);
        if (f.kind == FT::Array || f.kind == FT::VArray) {
            r["elem"] = kind_str(f.elem);
            if (!f.elem_name.empty()) r["elem_named"] = std::string(f.elem_name);
            if (f.kind == FT::Array) r["count"] = f.count;
        }
        if (f.str_cap) r["cap"] = f.str_cap;
        if (f.kind == FT::Enum) {
            json vs = json::array();
            rant::Schema::EnumVariant ev;
            for (uint16_t k = 0; c.schema().enum_variant((uint16_t)i, k, ev); k++)
                vs.push_back({ {"name", std::string(ev.name)}, {"value", ev.value} });
            r["variants"] = vs;
        }
        fields.push_back(r);
    }
    return { {"name", std::string(c.schema().name())}, {"hash", hex64(c.schema().hash())},
             {"dsl", c.schema().to_dsl()}, {"fields", fields} };
}

static json map_item(const rant::MapItem& m) {
    if (m.is_bool()) return m.as_bool();
    if (m.is_uint()) return m.as_uint();
    if (m.is_int()) return m.as_int();
    if (m.is_double()) return m.as_f64();
    if (m.is_string()) return m.as_string();
    if (m.is_array()) { json a = json::array(); for (const auto& e : m.as_array()) a.push_back(map_item(e)); return a; }
    if (m.is_map()) { json o = json::object(); for (const auto& kv : m.as_map()) o[kv.first] = map_item(kv.second); return o; }
    return nullptr;
}

static const char* entity_kind(rant::EntityKind k) {
    switch (k) {
    case rant::EntityKind::Topic: return "topic"; case rant::EntityKind::Function: return "function";
    case rant::EntityKind::Task: return "task"; case rant::EntityKind::Variable: return "variable";
    }
    return "?";
}

static json entity_json(const rant::Entity& e) {
    json j = { {"kind", entity_kind(e.kind)}, {"name", e.name}, {"provides", e.provides}, {"consumes", e.consumes},
               {"reliable", e.reliable}, {"providers", e.providers}, {"consumers", e.consumers},
               {"generation", hex64(e.generation)} };
    if (e.providers) j["provider"] = e.provider;
    if (!e.from.empty()) j["from"] = e.from;
    if (e.kind == rant::EntityKind::Variable) { j["writable"] = e.writable; j["forceable"] = e.forceable; }
    if (e.kind == rant::EntityKind::Task) { j["cancellable"] = e.cancellable; j["exclusive"] = e.exclusive; }
    if (e.multi) j["multi"] = true;
    if (e.incomplete) j["incomplete"] = true;
    if (e.conflict) j["conflict"] = true;
    if (!e.schema.empty()) j["schema"] = schema_block(Codec(e.schema));
    if (!e.rsp_schema.empty()) j["rsp"] = schema_block(Codec(e.rsp_schema));
    if (!e.progress_schema.empty()) j["prg"] = schema_block(Codec(e.progress_schema));
    return j;
}

static const char* call_status(rant::SendStatus s) {
    switch (s) {
    case rant::SendStatus::Ok: return "ok"; case rant::SendStatus::NoTopic: return "no such entity";
    case rant::SendStatus::TooBig: return "message too big"; case rant::SendStatus::BadRole: return "the role cannot do that";
    default: return "refused";
    }
}

static rant::LogLevel level_from(const std::string& s, bool& ok) {
    ok = true;
    if (s == "error") return rant::LogLevel::Error;
    if (s == "warn") return rant::LogLevel::Warn;
    if (s == "info") return rant::LogLevel::Info;
    ok = false;
    return rant::LogLevel::Info;
}

static const char* level_str(rant::LogLevel l) {
    return l == rant::LogLevel::Error ? "error" : l == rant::LogLevel::Warn ? "warn" : "info";
}

/* ---- lifecycle ---------------------------------------------------------------------- */

Session::Session(const Config& cfg, std::string token) : cfg_(cfg), token_(std::move(token)) {
    sender_ = std::make_unique<Sender>(
        [this](uint16_t id, const std::string& frame) { return carrier(id, frame); },
        [this] { tick(); }, kChunk);
    sender_->on_drop = [this](uint16_t id, uint32_t n) {
        event({ {"event", "msg_lost"}, {"id", id}, {"peer", 0}, {"count", n},
                {"text", "the bridge dropped best effort messages for a slow client"} });
    };
}

Session::~Session() {
    std::shared_ptr<Rtc> rtc;
    std::map<uint32_t, rant::Deferred<rant::Bytes>> parked;
    std::map<uint32_t, std::shared_ptr<ParkedTask>> tasks;
    {
        std::lock_guard<std::mutex> g(mu_);
        attached_ = false;      /* tick goes quiet while the node closes */
        rtc = std::move(rtc_);
        parked = std::move(parked_);
        tasks = std::move(parked_tasks_);
    }
    { std::lock_guard<std::mutex> g(ws_mu_); ws_ = nullptr; }
    tunnel_.reset();    /* its thread delivers into this session, so it stops first */
    rtc.reset();
    /* answer what waits on the client while the node is alive, so callers hear it now */
    for (auto& kv : parked) kv.second.fail("the bridge client went away");
    for (auto& kv : tasks) kv.second->pt.complete_cancelled("the bridge client went away");
    node_.reset();      /* stops the service thread and says bye before the handles go */
    sender_.reset();    /* after the node: a message delivered until it closes still goes to the sender */
    ents_.clear();
}

json Session::open(const json& req) {
    probe_ms_ = std::max(10u, req.value("probe_interval_ms", 250u));
    if (req.value("tunnel", false)) {
        /* the client runs its own node and borrows this host's sockets */
        name_ = req.value("name", std::string("tunnel"));
        tunnel_ = std::make_unique<Tunnel>([this](const std::string& batch) { to_client_udp(batch); });
        return reopen_reply();
    }
    rant::NodeOptions o;
    o.domain = (uint16_t)req.value("domain", 0);
    o.prefix = req.value("prefix", std::string());
    o.max_topics = (uint16_t)req.value("max_topics", 64);
    o.fetch_details = req.value("fetch_details", false);
    o.match_wait = milliseconds(req.value("match_wait_ms", 0));
    o.disable_shm = req.value("disable_shm", false);
    o.disable_logs = req.value("disable_logs", false);
    o.disable_meta = req.value("disable_meta", false);
    o.disable_error_logs = req.value("disable_error_logs", false);
    o.multicast_interface = req.value("multicast_interface", std::string());
    for (const auto& s : req.value("seed_peers", std::vector<std::string>{})) o.seed_peers.push_back(s);
    o.unicast_only = req.value("unicast_only", false);
    o.self_ip = req.value("self_ip", std::string());
    o.advertise_port = (uint16_t)req.value("advertise_port", 0);
    o.fragment_size = (uint16_t)req.value("fragment_size", 0);
    o.recv_buffer_bytes = req.value("recv_buffer_bytes", 0u);
    o.send_buffer_bytes = req.value("send_buffer_bytes", 0u);
    o.announce_interval = milliseconds(req.value("announce_interval_ms", 0));
    o.peer_timeout = milliseconds(req.value("peer_timeout_ms", 0));
    /* one bridge holds many nodes and each sees all its siblings, so Rant's 16 fills before the LAN does */
    o.max_peers = (uint16_t)req.value("max_peers", 256);
    probe_ms_ = std::max(10u, req.value("probe_interval_ms", 250u));

    std::string name = req.value("name", std::string());
    if (name.empty()) {
        char b[16];
        std::snprintf(b, sizeof b, "web-%08x", (unsigned)std::random_device{}());
        name = b;
    }
    try { node_.emplace(name, o); }
    catch (const rant::Error& e) { throw std::runtime_error(std::string("the node did not open: ") + e.what()); }
    node_->on_event([this](const rant::Event& ev) {
        json e = { {"text", ev.to_string()} };
        switch (ev.kind()) {
        case rant::EventKind::PeerUp: e["event"] = "peer_up"; e["peer"] = ev.peer(); e["peer_name"] = std::string(ev.peer_name()); break;
        case rant::EventKind::PeerDown: e["event"] = "peer_down"; e["peer"] = ev.peer(); e["peer_name"] = std::string(ev.peer_name()); break;
        case rant::EventKind::PeerInterest: e["event"] = "peer_interest"; e["peer"] = ev.peer(); break;
        case rant::EventKind::MessageLost:
            e["event"] = "msg_lost"; e["peer"] = ev.peer(); e["topic"] = std::string(ev.topic_name());
            e["first"] = ev.lost_first(); e["count"] = ev.lost_count();
            break;
        default:
            e["event"] = "error"; e["error"] = (int)ev.error();
            if (!ev.topic_name().empty()) e["topic"] = std::string(ev.topic_name());
            if (ev.peer()) e["peer"] = ev.peer();
            break;
        }
        event(std::move(e));
    });
    name_ = name;
    return reopen_reply();
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

json Session::reopen_reply() const {
    json r = { {"proto", kProtocol}, {"name", name_}, {"webrtc", cfg_.webrtc} };
    if (tunnel_) {
        json ifs = json::array();
        for (const Tunnel::Iface& f : Tunnel::interfaces()) ifs.push_back({ {"addr", dotted(f.addr)}, {"mask", dotted(f.mask)} });
        /* a POSIX host hands group traffic only to a socket bound to any address */
#ifdef _WIN32
        const bool group_binds_any = false;
#else
        const bool group_binds_any = true;
#endif
        r["tunnel"] = { {"interfaces", ifs}, {"group_binds_any", group_binds_any} };
    }
    return r;
}

void Session::attach(ix::WebSocket* ws) {
    std::shared_ptr<Rtc> old;     /* a returning client negotiates WebRTC afresh */
    { std::lock_guard<std::mutex> g(ws_mu_); ws_ = ws; }
    {
        std::lock_guard<std::mutex> g(mu_);
        attached_ = true;
        detached_at_ = {};
        old = std::move(rtc_);
        reasm_ = Reassembly{};
        link_.out.clear();
        link_.last_pong = clock_type::now();
        link_.missed = 0;
        for (auto& kv : ents_) kv.second->last_match = -1;   /* the client hears every match state again */
    }
    sender_->set_attached(true);
}

void Session::detach(ix::WebSocket* ws) {
    std::shared_ptr<Rtc> rtc;
    {
        std::lock_guard<std::mutex> g(ws_mu_);
        if (ws_ != ws) return;
        ws_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> g(mu_);
        attached_ = false;
        detached_at_ = clock_type::now();
        rtc = std::move(rtc_);
    }
    sender_->set_attached(false);
}

milliseconds Session::detached_for() const {
    std::lock_guard<std::mutex> g(mu_);
    if (attached_ || detached_at_ == clock_type::time_point{}) return milliseconds(0);
    return std::chrono::duration_cast<milliseconds>(clock_type::now() - detached_at_);
}

/* ---- sending ------------------------------------------------------------------------ */

void Session::send_json(const json& j) {
    std::string text = j.dump(-1, ' ', false, json::error_handler_t::replace);
    std::lock_guard<std::mutex> g(ws_mu_);
    if (ws_) ws_->send(text, false);
}

void Session::reply(const json& seq, json extra) {
    extra["op"] = "reply"; extra["seq"] = seq; extra["ok"] = true;
    send_json(extra);
}

void Session::reply_error(const json& seq, const std::string& why) {
    send_json({ {"op", "reply"}, {"seq", seq}, {"ok", false}, {"error", why} });
}

void Session::event(json e) {
    e["op"] = "event";
    send_json(e);
}

/* The entity's data channel when WebRTC is up and has room, else the WebSocket. */
bool Session::carrier(uint16_t id, const std::string& frame) {
    std::shared_ptr<Rtc> rtc;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (!attached_) return false;
        rtc = rtc_;
    }
    if (rtc && rtc->up() && rtc->send(id, frame)) return true;
    std::lock_guard<std::mutex> g(ws_mu_);
    if (!ws_) return false;
    if (ws_->bufferedAmount() > (64u << 10)) return false;
    ws_->sendBinary(frame);
    return true;
}

void Session::deliver(Entity& e, const Codec& codec, Outgoing m, rant::Bytes data, bool video) {
    if (codec.typed() && !e.raw && data.size()) {
        int skip = -1;
        if (video) {
            std::shared_ptr<Rtc> rtc;
            { std::lock_guard<std::mutex> g(mu_); rtc = rtc_; }
            if (rtc) skip = rtc->route_video(e.id, data, codec, m.written, m.capture);
        }
        cbor::Writer w;
        if (!codec.encode(data, w, skip)) {
            event({ {"event", "error"}, {"id", e.id}, {"text", "a message from the mesh did not match its schema"} });
            return;
        }
        m.payload = std::move(w.out);
    } else m.payload.assign((const char*)data.data(), data.size());
    if (sender_->push(e.id, std::move(m))) return;
    /* a reliable lane is full: this client cannot keep up */
    std::lock_guard<std::mutex> g(ws_mu_);
    if (ws_) {
        std::fprintf(stderr, "rant-bridge: %s cannot keep up with '%s', closing it\n", name_.c_str(), e.name.c_str());
        ws_->close(1008, "slow consumer");
    }
    else overflowed_ = true;
}

void Session::tick() {
    auto now = clock_type::now();
    /* pings: the client's round trip, and the liveness that detaches a silent client */
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
            /* the payload tells the client what the bridge measures of its link */
            cbor::Writer w;
            w.map(4);
            w.text("us"); w.uint(link_.last_us);
            w.text("jitter_us"); w.uint(link_.jitter_us);
            w.text("samples"); w.uint(link_.samples);
            w.text("missed"); w.uint(link_.missed);
            Frame f; f.op = OpPing; f.seq = seq; f.written = now_us(); f.payload = w.out;
            ping = build_frame(f);
        }
        auto silent = std::chrono::duration_cast<milliseconds>(now - link_.last_pong);
        link_.missed = (uint32_t)(silent.count() > (long long)probe_ms_ / 2 ? (silent.count() - probe_ms_ / 2) / probe_ms_ : 0);
        drop = silent > milliseconds(cfg_.detach_ms) && busy_ == 0;
    }
    if (!ping.empty()) {
        /* a ping skips the room check: left out while data drains, it would read as silence */
        std::shared_ptr<Rtc> rtc;
        { std::lock_guard<std::mutex> g(mu_); rtc = rtc_; }
        if (!(rtc && rtc->up() && rtc->send(0, ping))) {
            std::lock_guard<std::mutex> g(ws_mu_);
            if (ws_) ws_->sendBinary(ping);
        }
    }
    if (drop) {
        std::lock_guard<std::mutex> g(ws_mu_);
        if (ws_) {
            std::fprintf(stderr, "rant-bridge: %s stopped answering pings, closing it\n", name_.c_str());
            ws_->close(4008, "no answer to pings");
        }
        return;
    }

    /* match state, pushed when it changes. Rant is asked outside the lock */
    std::vector<std::shared_ptr<Entity>> ents;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto& kv : ents_) ents.push_back(kv.second);
    }
    std::vector<json> pushes;
    for (auto& ep : ents) {
        Entity& e = *ep;
        {
            int count = 0; bool ready = true;
            switch (e.kind) {
            case Entity::Pub: count = e.pub->match_count(); ready = e.pub->ready(); break;
            case Entity::Sub: count = e.sub->match_count(); break;
            case Entity::FnDef: count = e.fndef->match_count(); break;
            case Entity::FnRemote: count = e.fnrem->match_count(); ready = count > 0; break;
            case Entity::TaskDef: count = e.taskdef->match_count(); break;
            case Entity::TaskRemote: count = e.taskrem->match_count(); ready = count > 0; break;
            case Entity::VarDef: case Entity::VarRemote: count = e.var()->match_count(); break;
            }
            int packed = count * 2 + (ready ? 1 : 0);
            std::lock_guard<std::mutex> g(mu_);
            if (packed == e.last_match) continue;
            e.last_match = packed;
            pushes.push_back({ {"op", "match"}, {"id", e.id}, {"count", count}, {"ready", ready} });
        }
    }
    for (auto& p : pushes) send_json(p);

    /* a queue that waits too long is never silent */
    auto stats = sender_->stats();
    std::vector<json> warns;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto& kv : stats) {
            auto it = ents_.find(kv.first);
            if (it == ents_.end()) continue;
            bool late = kv.second.oldest_us > (uint64_t)cfg_.queue_delay_warn_ms * 1000u;
            if (late && !it->second->delayed)
                warns.push_back({ {"op", "warning"}, {"kind", "queue_delay"}, {"id", kv.first},
                                  {"text", "messages for '" + it->second->name + "' wait " + std::to_string(kv.second.oldest_us / 1000) + " ms to be sent"} });
            it->second->delayed = late;
        }
    }
    for (auto& w : warns) send_json(w);
}

std::shared_ptr<Entity> Session::find(uint16_t id) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = ents_.find(id);
    return it == ents_.end() ? nullptr : it->second;
}

/* ---- control ops -------------------------------------------------------------------- */

/* While a client message is handled, and the moment it is done, the client counts as heard:
 * its pongs may wait unread behind a send blocked in Rant. */
struct Session::Busy {
    Session& s;
    explicit Busy(Session& ss) : s(ss) { s.busy_++; s.heard(); }
    ~Busy() { s.heard(); s.busy_--; }
};

void Session::heard() {
    std::lock_guard<std::mutex> g(mu_);
    link_.last_pong = clock_type::now();
}

void Session::on_text(const json& req) {
    Busy busy(*this);
    json seq = req.value("seq", json(0));
    std::string op = req.value("op", "");
    try {
        if (op == "rtc") { op_rtc(req, seq); return; }
        if (op.rfind("udp_", 0) == 0) { op_udp(op, req, seq); return; }
        if (!node_) { reply_error(seq, "a tunnel session has no node, its client runs its own"); return; }
        if (op == "create") op_create(req, seq);
        else if (op == "close") op_close(req, seq);
        else if (op == "refresh") op_refresh(req, seq);
        else if (op == "drain") {
            auto e = find((uint16_t)req.value("id", 0));
            if (!e || e->kind != Entity::Pub) { reply_error(seq, "no such publisher"); return; }
            reply(seq, { {"drained", e->pub->drain(milliseconds(req.value("timeout_ms", 1000)))} });
        }
        else if (op == "settle") {
            int ms = req.value("timeout_ms", -1);
            bool ok = ms < 0 ? node_->settle() : node_->settle(milliseconds(ms));
            reply(seq, { {"settled", ok} });
        }
        else if (op == "cancel") op_cancel(req, seq);
        else if (op == "log") {
            bool ok;
            rant::LogLevel l = level_from(req.value("level", ""), ok);
            if (!ok) { reply_error(seq, "level is error, warn or info"); return; }
            rant::SendStatus s = node_->log(l, req.value("text", ""));
            if (s != rant::SendStatus::Ok) { reply_error(seq, call_status(s)); return; }
            reply(seq, {});
        }
        else if (op == "log_subscribe") op_log_subscribe(req, seq);
        else if (op == "peers") op_peers(seq);
        else if (op == "entities" || op == "peer_entities" || op == "mesh" || op == "find" || op == "epoch") op_reflect(op, req, seq);
        else if (op == "meta") op_meta(req, seq);
        else if (op == "stats") op_stats(seq);
        else if (op == "rtc") op_rtc(req, seq);
        else reply_error(seq, "unknown op '" + op + "'");
    } catch (const std::exception& e) {
        reply_error(seq, e.what());
    }
}

static rant::Qos topic_qos(const json& req) {
    rant::Qos q;
    q.reliability = req.value("reliable", false) ? rant::Reliability::Reliable : rant::Reliability::BestEffort;
    q.keep_last = (uint16_t)req.value("keep_last", 0);
    q.catch_up = (uint16_t)req.value("catch_up", 0);
    q.max_message_bytes = req.value("max_message_bytes", 0u);
    q.heartbeat = milliseconds(req.value("heartbeat_ms", 0));
    q.repair_delay = milliseconds(req.value("repair_delay_ms", 0));
    q.backpressure_wait = milliseconds(req.value("backpressure_wait_ms", 0));
    q.shm_max_bytes = req.value("shm_max_bytes", 0u);
    q.queue_bytes = req.value("queue_bytes", 0u);
    q.max_rate_hz = (uint16_t)req.value("max_rate_hz", 0);
    q.no_timestamp = req.value("no_timestamp", false);
    q.reflect_from_mesh = req.value("reflect", false);
    return q;
}

json Session::schemas(Entity& e, bool reflect) {
    json r = { {"id", e.id}, {"reliable", e.reliable} };
    if (reflect) r["reflected"] = e.value.typed() || e.req.typed();
    if (e.raw) r["raw"] = true;
    switch (e.kind) {
    case Entity::Pub: case Entity::Sub: case Entity::VarDef: case Entity::VarRemote:
        r["schema"] = schema_block(e.value);
        break;
    default:
        r["req"] = schema_block(e.req);
        r["rsp"] = schema_block(e.rsp);
        if (e.kind == Entity::TaskDef || e.kind == Entity::TaskRemote) r["prg"] = schema_block(e.prg);
        break;
    }
    return r;
}

void Session::op_create(const json& req, const json& seq) {
    int id = req.value("id", -1);
    std::string kind = req.value("kind", ""), name = req.value("name", "");
    if (id < 1 || id > 0xFFFE) { reply_error(seq, "id must be 1 to 65534"); return; }
    if (name.empty()) { reply_error(seq, "missing name"); return; }
    if (find((uint16_t)id)) { reply_error(seq, "id already in use"); return; }

    auto compile = [&](const char* key) -> rant::Schema {
        std::string text = req.value(key, std::string());
        return text.empty() ? rant::Schema() : node_->schema(text);
    };
    bool reflect = req.value("reflect", false);
    auto e = std::make_shared<Entity>();
    Entity* ent = e.get();
    e->id = (uint16_t)id;
    e->name = name;
    e->raw = req.value("raw", false);

    rant::FunctionOptions fo;
    fo.timeout = milliseconds(req.value("timeout_ms", 0));
    fo.backpressure_wait = milliseconds(req.value("backpressure_wait_ms", 0));
    fo.keep_last = (uint16_t)req.value("keep_last", 0);
    fo.reflect_from_mesh = reflect;
    rant::TaskOptions to;
    to.progress_best_effort = req.value("progress_best_effort", false);
    to.progress_keep_last = (uint16_t)req.value("progress_keep_last", 0);
    to.no_cancel = req.value("no_cancel", false);
    to.exclusive = req.value("exclusive", false);
    to.multi = req.value("multi", false);
    to.timeout = fo.timeout; to.backpressure_wait = fo.backpressure_wait; to.keep_last = fo.keep_last;
    to.reflect_from_mesh = reflect;

    if (kind == "publisher" || kind == "subscriber") {
        rant::Qos q = topic_qos(req);
        rant::Schema s = compile("schema");
        e->reliable = q.reliability == rant::Reliability::Reliable;
        if (kind == "publisher") {
            e->kind = Entity::Pub;
            e->pub.emplace(node_->publisher<rant::Bytes>(name, q, s));
            e->value = Codec(e->pub->schema());
        } else {
            e->kind = Entity::Sub;
            e->sub.emplace(node_->subscriber<rant::Bytes>(name, [this, ent](const rant::MessageView& m) {
                Outgoing o; o.op = OpData; o.peer = m.publisher_id(); o.written = m.written_us(); o.capture = m.capture_us();
                deliver(*ent, ent->value, std::move(o), m.data(), true);
            }, q, s));
            e->value = Codec(e->sub->schema());
        }
    } else if (kind == "function_definition") {
        e->kind = Entity::FnDef;
        e->fndef.emplace(node_->function_definition<rant::Bytes, rant::Bytes>(name, [this, ent](rant::Request<rant::Bytes>& r) {
            Outgoing o; o.op = OpCall; o.peer = r.caller(); o.written = r.written_us(); o.text = std::string(r.caller_name());
            rant::Bytes data = r.data();
            std::string copy((const char*)data.data(), data.size());
            rant::Deferred<rant::Bytes> d = r.defer();
            {
                std::lock_guard<std::mutex> g(mu_);
                o.seq = ++next_req_;
                parked_.emplace(o.seq, std::move(d));
            }
            deliver(*ent, ent->req, std::move(o), rant::Bytes(copy.data(), copy.size()), false);
        }, fo, compile("req"), compile("rsp")));
        e->req = Codec(e->fndef->request_schema()); e->rsp = Codec(e->fndef->response_schema());
    } else if (kind == "remote_function") {
        e->kind = Entity::FnRemote;
        e->fnrem.emplace(node_->remote_function<rant::Bytes, rant::Bytes>(name, fo, compile("req"), compile("rsp")));
        e->req = Codec(e->fnrem->request_schema()); e->rsp = Codec(e->fnrem->response_schema());
    } else if (kind == "task_definition") {
        e->kind = Entity::TaskDef;
        e->taskdef.emplace(node_->task_definition<rant::Bytes, rant::Bytes, rant::Bytes>(name, [this, ent](rant::TaskRequest<rant::Bytes, rant::Bytes>& r) {
            Outgoing o; o.op = OpCall; o.peer = r.caller(); o.written = r.written_us(); o.text = std::string(r.caller_name());
            rant::Bytes data = r.data();
            std::string copy((const char*)data.data(), data.size());
            auto tk = std::make_shared<ParkedTask>();
            tk->entity = ent->id;
            tk->pt = r.defer();           /* RUNNING to the caller */
            {
                std::lock_guard<std::mutex> g(mu_);
                o.seq = ++next_req_;
                parked_tasks_.emplace(o.seq, tk);
            }
            deliver(*ent, ent->req, std::move(o), rant::Bytes(copy.data(), copy.size()), false);
        }, to, compile("req"), compile("prg"), compile("rsp")));
        /* the token names no request through the wrapper, so look for the newly cancelled one */
        e->taskdef->on_cancel([this, ent](uint64_t) {
            std::vector<uint32_t> hit;
            {
                std::lock_guard<std::mutex> g(mu_);
                for (auto& kv : parked_tasks_)
                    if (kv.second->entity == ent->id && !kv.second->cancel_sent && kv.second->pt.cancelled()) {
                        kv.second->cancel_sent = true;
                        hit.push_back(kv.first);
                    }
            }
            for (uint32_t s : hit) { Outgoing o; o.op = OpCancel; o.seq = s; sender_->push(ent->id, std::move(o)); }
        });
        e->req = Codec(e->taskdef->request_schema()); e->prg = Codec(e->taskdef->progress_schema());
        e->rsp = Codec(e->taskdef->response_schema());
    } else if (kind == "remote_task") {
        e->kind = Entity::TaskRemote;
        e->taskrem.emplace(node_->remote_task<rant::Bytes, rant::Bytes, rant::Bytes>(name, to, compile("req"), compile("prg"), compile("rsp")));
        e->req = Codec(e->taskrem->request_schema()); e->prg = Codec(e->taskrem->progress_schema());
        e->rsp = Codec(e->taskrem->response_schema());
    } else if (kind == "variable_definition" || kind == "remote_variable") {
        rant::VariableOptions<rant::Bytes> vo;
        vo.read_only = req.value("read_only", false);
        vo.allow_force = req.value("allow_force", false);
        vo.catch_up = (uint16_t)req.value("catch_up", 0);
        vo.keep_last = (uint16_t)req.value("keep_last", 0);
        vo.backpressure_wait = milliseconds(req.value("backpressure_wait_ms", 0));
        vo.reflect_from_mesh = reflect;
        rant::Schema s = compile("schema");
        if (kind == "variable_definition") {
            e->kind = Entity::VarDef;
            e->vardef.emplace(node_->variable_definition<rant::Bytes>(name, vo, s));
        } else {
            e->kind = Entity::VarRemote;
            e->varrem.emplace(node_->remote_variable<rant::Bytes>(name, vo, s));
        }
        e->value = Codec(e->var()->schema());
    } else {
        reply_error(seq, "unknown kind '" + kind + "'");
        return;
    }

    size_t qb = req.value("queue_bytes", 0u);
    sender_->add_lane(e->id, e->reliable, e->reliable ? std::max(qb, kReliableCap) : (qb ? qb : kBestEffortCap));
    std::shared_ptr<Rtc> rtc;
    {
        std::lock_guard<std::mutex> g(mu_);
        ents_.emplace(e->id, std::move(e));
        rtc = rtc_;
    }
    if (rtc) rtc->add_channel(ent->id, ent->reliable);
    reply(seq, schemas(*ent, reflect));

    /* variables push from here on: registration replays the current value */
    if (rant::VariableDefinition<rant::Bytes>* v = ent->var()) {
        auto push = [this, ent](const rant::VariableUpdate& u, uint8_t evbit) {
            Outgoing o; o.op = OpVar; o.flags = (uint8_t)((u.forced() ? 1 : 0) | evbit);
            o.peer = u.source(); o.written = u.written_us();
            deliver(*ent, ent->value, std::move(o), u.value(), true);
        };
        v->on_change([push](const rant::VariableUpdate& u) { push(u, 0); });
        if (req.value("on_write", false)) v->on_write([push](const rant::VariableUpdate& u) { push(u, 2); });
    }
}

void Session::op_close(const json& req, const json& seq) {
    uint16_t id = (uint16_t)req.value("id", 0);
    std::shared_ptr<Entity> e;
    std::shared_ptr<Rtc> rtc;
    std::vector<std::shared_ptr<ParkedTask>> cancels;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = ents_.find(id);
        if (it != ents_.end()) {
            e = std::move(it->second);
            ents_.erase(it);
            rtc = rtc_;
            for (auto p = parked_tasks_.begin(); p != parked_tasks_.end();)
                if (p->second->entity == id) { cancels.push_back(p->second); p = parked_tasks_.erase(p); } else ++p;
        }
    }
    if (!e) { reply_error(seq, "no such entity"); return; }
    sender_->remove_lane(id);
    if (rtc) rtc->remove_channel(id);
    for (auto& t : cancels) t->pt.complete_cancelled("the client closed the definition");
    e.reset();       /* the handle's destructor answers its own outstanding calls */
    reply(seq, {});
}

void Session::op_refresh(const json& req, const json& seq) {
    auto e = find((uint16_t)req.value("id", 0));
    if (!e) { reply_error(seq, "no such entity"); return; }
    bool retyped = false;
    switch (e->kind) {
    case Entity::Pub: retyped = e->pub->refresh(); e->value = Codec(e->pub->schema()); break;
    case Entity::Sub: retyped = e->sub->refresh(); e->value = Codec(e->sub->schema()); break;
    case Entity::FnDef: retyped = e->fndef->refresh(); e->req = Codec(e->fndef->request_schema()); e->rsp = Codec(e->fndef->response_schema()); break;
    case Entity::FnRemote: retyped = e->fnrem->refresh(); e->req = Codec(e->fnrem->request_schema()); e->rsp = Codec(e->fnrem->response_schema()); break;
    case Entity::TaskDef: retyped = e->taskdef->refresh(); e->req = Codec(e->taskdef->request_schema()); e->prg = Codec(e->taskdef->progress_schema()); e->rsp = Codec(e->taskdef->response_schema()); break;
    case Entity::TaskRemote: retyped = e->taskrem->refresh(); e->req = Codec(e->taskrem->request_schema()); e->prg = Codec(e->taskrem->progress_schema()); e->rsp = Codec(e->taskrem->response_schema()); break;
    case Entity::VarDef: case Entity::VarRemote: retyped = e->var()->refresh(); e->value = Codec(e->var()->schema()); break;
    }
    json r = schemas(*e, true);
    r["retyped"] = retyped;
    reply(seq, r);
}

void Session::op_cancel(const json& req, const json& seq) {
    uint32_t call = req.value("call", 0u);
    uint16_t id = 0; uint32_t cid = 0;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = task_calls_.find(call);
        if (it != task_calls_.end()) { id = it->second.first; cid = it->second.second; }
    }
    if (!id) { reply(seq, { {"status", "not_pending"} }); return; }
    auto e = find(id);
    if (!e || e->kind != Entity::TaskRemote) { reply(seq, { {"status", "not_pending"} }); return; }
    rant::SendStatus s = e->taskrem->cancel(cid);
    reply(seq, { {"status", s == rant::SendStatus::Ok ? "ok" : s == rant::SendStatus::BadRole ? "no_cancel"
                            : s == rant::SendStatus::State ? "not_pending" : "error"} });
}

void Session::op_log_subscribe(const json& req, const json& seq) {
    uint8_t mask = 0;
    for (const auto& l : req.value("levels", std::vector<std::string>{ "error", "warn", "info" })) {
        bool ok;
        rant::LogLevel lv = level_from(l, ok);
        if (!ok) { reply_error(seq, "level is error, warn or info"); return; }
        mask |= (uint8_t)(1u << (int)lv);
    }
    bool hook;
    {
        std::lock_guard<std::mutex> g(mu_);
        log_levels_ |= mask;
        hook = !log_hooked_;
        log_hooked_ = true;
    }
    if (hook && !node_->on_log([this](const rant::LogLine& l) {
            {
                std::lock_guard<std::mutex> g(mu_);
                if (!(log_levels_ & (1u << (int)l.level))) return;
            }
            send_json({ {"op", "log"}, {"level", level_str(l.level)}, {"node", std::string(l.node)},
                        {"wall_us", l.wall_us}, {"mono_us", l.mono_us}, {"recv_us", l.recv_us},
                        {"written_us", l.written_us}, {"text", std::string(l.text)} });
        })) {
        reply_error(seq, "logs are disabled on this node");
        return;
    }
    reply(seq, {});
}

void Session::op_peers(const json& seq) {
    uint32_t link_us, link_jitter, samples, missed;
    {
        std::lock_guard<std::mutex> g(mu_);
        link_us = link_.last_us; link_jitter = link_.jitter_us; samples = link_.samples; missed = link_.missed;
    }
    json arr = json::array();
    for (const rant::Peer& p : node_->reflection().peers()) {
        json mesh = { {"us", p.rtt_us}, {"jitter_us", p.rtt_jitter_us}, {"samples", p.rtt_samples} };
        /* the client's view: its own hop to the bridge on top of a measured one, never alone */
        json full = p.rtt_samples ? json{ {"us", p.rtt_us + link_us}, {"jitter_us", p.rtt_jitter_us + link_jitter},
                                          {"samples", std::min(p.rtt_samples, samples)} }
                                  : mesh;
        char uuid[33];
        for (size_t i = 0; i < p.uuid.size(); i++) std::snprintf(uuid + i * 2, 3, "%02x", p.uuid[i]);
        arr.push_back({ {"id", p.id}, {"uuid", uuid}, {"name", p.name}, {"address", p.address}, {"active", p.active},
                        {"epoch", p.epoch}, {"catching_up", p.catching_up}, {"fragment_size", p.fragment_size},
                        {"last_heard_us", p.last_heard_us}, {"rtt", full}, {"mesh_rtt", mesh},
                        {"rtt_min_us", p.rtt_min_us}, {"missed", missed} });
    }
    reply(seq, { {"peers", arr} });
}

void Session::op_reflect(const std::string& op, const json& req, const json& seq) {
    rant::Reflection r = node_->reflection();
    if (op == "epoch") { reply(seq, { {"epoch", r.epoch()} }); return; }
    if (op == "find") {
        std::string k = req.value("kind", "");
        rant::EntityKind kind = k == "topic" ? rant::EntityKind::Topic : k == "function" ? rant::EntityKind::Function
                              : k == "task" ? rant::EntityKind::Task : rant::EntityKind::Variable;
        if (k != "topic" && k != "function" && k != "task" && k != "variable") { reply_error(seq, "kind is topic, function, task or variable"); return; }
        auto e = r.find(kind, req.value("name", ""));
        reply(seq, { {"entity", e ? entity_json(*e) : json(nullptr)}, {"epoch", r.epoch()} });
        return;
    }
    std::vector<rant::Entity> list = op == "mesh" ? r.mesh() : r.entities(op == "entities" ? 0u : req.value("peer", 0u));
    json arr = json::array();
    for (const auto& e : list) arr.push_back(entity_json(e));
    reply(seq, { {"entities", arr}, {"epoch", r.epoch()} });
}

void Session::op_meta(const json& req, const json& seq) {
    rant::SendStatus s = node_->reflection().meta_async(req.value("peer", 0u), [this, seq](const rant::MetaSnapshot& m) {
        json o = json::object();
        for (const auto& kv : m.info) o[kv.first] = map_item(kv.second);
        reply(seq, { {"valid", m.valid}, {"status", (int)m.status}, {"provider", m.provider}, {"info", o} });
    }, req.value("sections", 0u));
    if (s != rant::SendStatus::Ok) reply_error(seq, std::string("meta request failed: ") + call_status(s));
}

void Session::op_stats(const json& seq) {
    json ents = json::object();
    for (auto& kv : sender_->stats())
        ents[std::to_string(kv.first)] = { {"queued_bytes", kv.second.bytes}, {"queued_msgs", kv.second.msgs},
                                           {"oldest_age_us", kv.second.oldest_us}, {"dropped", kv.second.dropped} };
    json link;
    {
        std::lock_guard<std::mutex> g(mu_);
        json rtt = { {"us", link_.last_us}, {"jitter_us", link_.jitter_us}, {"samples", link_.samples}, {"at_us", link_.at_us} };
        link = { {"carrier", rtc_ && rtc_->up() ? "webrtc" : "websocket"}, {"rtt", rtt}, {"missed", link_.missed} };
    }
    reply(seq, { {"entities", ents}, {"link", link} });
}

void Session::op_rtc(const json& req, const json& seq) {
    if (!cfg_.webrtc) { reply_error(seq, "webrtc is off on this bridge"); return; }
    std::shared_ptr<Rtc> rtc;
    { std::lock_guard<std::mutex> g(mu_); rtc = rtc_; }
    if (req.contains("sdp")) {
        if (!rtc) { reply_error(seq, "start webrtc first"); return; }
        rtc->offer(req.value("sdp", ""), req.value("tracks", json::object()),
                   [this, seq](json answer) { reply(seq, std::move(answer)); });
        return;
    }
    if (req.contains("candidate")) {
        if (rtc) rtc->candidate(req.value("candidate", ""), req.value("mid", ""));
        reply(seq, {});
        return;
    }
    Rtc::Hooks h;
    h.push = [this](const json& j) { send_json(j); };
    h.frame = [this](const uint8_t* p, size_t n) { on_binary(p, n); };
    h.writable = [this] { sender_->wake(); };
    h.codec = [this](uint16_t id) -> const Codec* { auto e = find(id); return e ? &e->value : nullptr; };
    rtc = std::make_shared<Rtc>(cfg_, h);
    json r = rtc->start();
    std::vector<std::pair<uint16_t, bool>> chans;
    {
        std::lock_guard<std::mutex> g(mu_);
        rtc_ = rtc;
        for (auto& kv : ents_) chans.emplace_back(kv.first, kv.second->reliable);
    }
    for (auto& c : chans) rtc->add_channel(c.first, c.second);
    if (tunnel_) rtc->add_channel(kTunnelChannel, false);    /* datagrams: unordered, no retransmits */
    reply(seq, r);
}

/* ---- the tunnel --------------------------------------------------------------------- */

void Session::op_udp(const std::string& op, const json& req, const json& seq) {
    if (!tunnel_) { reply_error(seq, "this session has a node, not a tunnel"); return; }
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
        if (req.value("own_groups", false)) tunnel_->own_groups(sock);
        reply(seq, {});
    } else if (op == "udp_close") {
        tunnel_->close(sock);
        reply(seq, {});
    } else reply_error(seq, "unknown op '" + op + "'");
}

/* A batch of datagrams from the client's node, each out of this host's socket. */
void Session::on_udp(const uint8_t* p, size_t n) {
    if (!tunnel_) return;
    heard();
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

/* A batch for the client's node, on the tunnel channel while WebRTC is up, else the WebSocket.
 * One carrier only, so datagrams keep their order, and dropped past a socket's worth of
 * backlog, as UDP would drop them. */
void Session::to_client_udp(const std::string& batch) {
    std::shared_ptr<Rtc> rtc;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (!attached_) return;
        rtc = rtc_;
    }
    if (rtc && rtc->up() && rtc->has(kTunnelChannel)) { rtc->send(kTunnelChannel, batch, kTunnelBacklog); return; }
    std::lock_guard<std::mutex> g(ws_mu_);
    if (ws_ && ws_->bufferedAmount() < kTunnelBacklog) ws_->sendBinary(batch);
}

/* ---- frames from the client --------------------------------------------------------- */

void Session::on_binary(const uint8_t* p, size_t n) {
    if (n && p[0] == OpUdp) { on_udp(p, n); return; }
    Frame f;
    if (!parse_frame(p, n, f)) return;
    if (f.op == OpPong) { on_pong(f); return; }
    Busy busy(*this);
    std::string whole;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (!reasm_.add(f, whole, cfg_.max_message_bytes)) return;
    }
    auto e = find(f.id);
    if (f.op != OpResult && f.op != OpProgress && !e) {
        event({ {"event", "send_error"}, {"id", f.id}, {"error", "no such entity"} });
        return;
    }
    /* RESULT and PROGRESS answer a parked request, whose entity says how to decode */
    const Codec* codec = nullptr;
    if (f.op == OpData || f.op == OpVar) codec = &e->value;
    else if (f.op == OpCall) codec = &e->req;
    std::vector<uint8_t> bytes;
    rant::Bytes payload((const uint8_t*)whole.data(), whole.size());
    if (codec && codec->typed() && !e->raw && !whole.empty()) {
        cbor::Item v; std::string err;
        if (!cbor::parse(whole, v, err) || !codec->decode(v, bytes, err)) {
            event({ {"event", "send_error"}, {"id", f.id}, {"error", err} });
            if (f.op == OpCall) {     /* settle the caller's promise */
                Outgoing o; o.op = OpResult; o.flags = (uint8_t)rant::CallStatus::Cancelled; o.seq = f.seq; o.text = err;
                sender_->push(f.id, std::move(o));
            }
            return;
        }
        payload = rant::Bytes(bytes.data(), bytes.size());
    }
    rant::SendStatus s = rant::SendStatus::Ok;
    switch (f.op) {
    case OpData:
        if (e->kind != Entity::Pub) { s = rant::SendStatus::BadRole; break; }
        s = e->pub->send(payload, rant::types::Timestamp{ (int64_t)f.capture });
        break;
    case OpVar: {
        rant::VariableDefinition<rant::Bytes>* v = e->var();
        if (!v) { s = rant::SendStatus::BadRole; break; }
        s = f.flags == 0 ? v->set(payload) : f.flags == 1 ? v->force(payload) : v->unforce();
        break;
    }
    case OpCall: on_call(*e, f, payload); return;
    case OpResult: on_result(f, payload); return;
    case OpProgress: on_progress(f, payload); return;
    default: return;
    }
    if (s != rant::SendStatus::Ok) event({ {"event", "send_error"}, {"id", f.id}, {"code", (int)s}, {"error", call_status(s)} });
}

void Session::on_call(Entity& e, const Frame& f, rant::Bytes payload) {
    uint32_t call = f.seq;
    Entity* ent = &e;
    auto result = [this, ent, call](const rant::ResponseView<rant::Bytes>& rv) {
        Outgoing o; o.op = OpResult; o.flags = (uint8_t)rv.status(); o.seq = call; o.peer = rv.provider();
        o.written = rv.written_us(); o.text = std::string(rv.message());
        deliver(*ent, ent->rsp, std::move(o), rv.data(), false);
    };
    if (e.kind == Entity::FnRemote) {
        rant::SendStatus s = e.fnrem->call_async(payload, result);
        if (s != rant::SendStatus::Ok) {
            Outgoing o; o.op = OpResult; o.flags = (uint8_t)rant::CallStatus::Cancelled; o.seq = call; o.text = call_status(s);
            sender_->push(e.id, std::move(o));
        }
        return;
    }
    if (e.kind != Entity::TaskRemote) { event({ {"event", "send_error"}, {"id", e.id}, {"error", "not a remote function or task"} }); return; }
    { std::lock_guard<std::mutex> g(mu_); task_calls_[call] = { e.id, 0 }; }
    auto tc = e.taskrem->call_async(payload, [this, ent, call](const rant::ProgressView<rant::Bytes>& pv) {
        Outgoing o; o.op = OpProgress; o.seq = call; o.peer = pv.provider(); o.written = pv.written_us();
        deliver(*ent, ent->prg, std::move(o), pv.has_value() ? pv.data() : rant::Bytes(), false);
    }, [this, result, call](const rant::ResponseView<rant::Bytes>& rv) {
        { std::lock_guard<std::mutex> g(mu_); task_calls_.erase(call); }
        result(rv);
    });
    if (!tc.ok()) {
        { std::lock_guard<std::mutex> g(mu_); task_calls_.erase(call); }
        Outgoing o; o.op = OpResult; o.flags = (uint8_t)rant::CallStatus::Cancelled; o.seq = call; o.text = call_status(tc.status);
        sender_->push(e.id, std::move(o));
        return;
    }
    std::lock_guard<std::mutex> g(mu_);
    auto it = task_calls_.find(call);
    if (it != task_calls_.end()) it->second.second = tc.id;
}

/* the reply to a parked request: decoded with the definition's response or progress schema */
static bool decode_with(const Codec& c, rant::Bytes in, std::vector<uint8_t>& out, std::string& err) {
    if (!c.typed() || !in.size()) { out.assign(in.data(), in.data() + in.size()); return true; }
    cbor::Item v;
    return cbor::parse(std::string_view((const char*)in.data(), in.size()), v, err) && c.decode(v, out, err);
}

void Session::on_result(const Frame& f, rant::Bytes payload) {
    rant::Deferred<rant::Bytes> d;
    std::shared_ptr<ParkedTask> tk;
    auto e = find(f.id);
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = parked_.find(f.seq);
        if (it != parked_.end()) { d = std::move(it->second); parked_.erase(it); }
        else {
            auto t = parked_tasks_.find(f.seq);
            if (t == parked_tasks_.end()) return;      /* already answered, or the client is late */
            tk = t->second;
            parked_tasks_.erase(t);
        }
    }
    std::vector<uint8_t> bytes; std::string err;
    if (e && !decode_with(e->raw ? Codec() : e->rsp, payload, bytes, err)) {
        event({ {"event", "send_error"}, {"id", f.id}, {"error", err} });
        if (d.valid()) d.fail("the reply did not match the response schema: " + err);
        else tk->pt.fail("the reply did not match the response schema: " + err);
        return;
    }
    rant::Bytes rsp(bytes.data(), bytes.size());
    if (d.valid()) {
        if (f.flags == 0) d.complete(rsp, f.text); else d.fail(f.text, rsp);
        return;
    }
    if (f.flags == 0) tk->pt.complete(rsp, f.text);
    else if (f.flags == (uint8_t)rant::CallStatus::Cancelled) tk->pt.complete_cancelled(f.text, rsp);
    else tk->pt.fail(f.text, rsp);
}

void Session::on_progress(const Frame& f, rant::Bytes payload) {
    std::shared_ptr<ParkedTask> tk;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = parked_tasks_.find(f.seq);
        if (it == parked_tasks_.end()) return;
        tk = it->second;
    }
    auto e = find(tk->entity);
    std::vector<uint8_t> bytes; std::string err;
    if (e && !decode_with(e->raw ? Codec() : e->prg, payload, bytes, err)) {
        event({ {"event", "send_error"}, {"id", tk->entity}, {"error", err} });
        return;
    }
    rant::SendStatus s = tk->pt.progress(rant::Bytes(bytes.data(), bytes.size()));
    if (s != rant::SendStatus::Ok) event({ {"event", "send_error"}, {"id", tk->entity}, {"error", call_status(s)} });
}

void Session::on_pong(const Frame& f) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = link_.out.find(f.seq);
    if (it == link_.out.end()) return;
    auto now = clock_type::now();
    uint32_t rtt = (uint32_t)std::chrono::duration_cast<microseconds>(now - it->second).count();
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
    link_.at_us = now_us();
    link_.last_pong = now;
}

}
