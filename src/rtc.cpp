#include "rtc.hpp"

#include <rtc/rtc.hpp>

#include <chrono>
#include <cstdio>

namespace bridge {

using json = nlohmann::json;
using FT = rant::FieldType;
namespace d = rant::detail;

static const size_t kLowWater = 64u << 10;   /* a channel takes more once it drains under this */

/* One video line bound to the VideoFrame at path of one entity's values. */
struct Rtc::Binding {
    std::string mid, path;
    bool keep_data = false;
    std::string f_codec, f_key, f_pts, f_data;
    int data_field = -1;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtp;
    int codec = 0, sep = -1;            /* the packetizer installed now */
    bool keyed = false;                 /* a keyframe opened the stream since the last restart */
    uint8_t pt[5] = { 0, 0, 0, 0, 0 };  /* the browser's payload type per codec, 0 = not offered */
};

Rtc::Rtc(const Config& cfg, Hooks hooks) : cfg_(cfg), hooks_(std::move(hooks)) {}

Rtc::~Rtc() {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::map<uint16_t, std::shared_ptr<rtc::DataChannel>> chans;
    std::map<uint16_t, std::shared_ptr<Binding>> binds;
    {
        std::lock_guard<std::mutex> g(mu_);
        pc = std::move(pc_); chans = std::move(channels_); binds = std::move(bindings_);
    }
    up_ = false;
    for (auto& kv : chans) { kv.second->resetCallbacks(); kv.second->close(); }
    for (auto& kv : binds) if (kv.second->track) { kv.second->track->resetCallbacks(); kv.second->track->close(); }
    if (pc) { pc->resetCallbacks(); pc->close(); }
}

static std::string ice_url(const IceServer& s) {
    if (s.username.empty()) return s.urls;
    /* libdatachannel takes credentials inside the url */
    auto colon = s.urls.find(':');
    return s.urls.substr(0, colon + 1) + s.username + ":" + s.credential + "@" + s.urls.substr(colon + 1);
}

json Rtc::start() {
    rtc::Configuration c;
    for (const auto& s : cfg_.ice) c.iceServers.emplace_back(ice_url(s));
    c.disableAutoNegotiation = true;     /* every answer is ours to trigger */
    c.forceMediaTransport = true;        /* video lines may arrive in a later offer */
    if (cfg_.rtc_port_lo) { c.portRangeBegin = cfg_.rtc_port_lo; c.portRangeEnd = cfg_.rtc_port_hi; }
    c.mtu = cfg_.mtu;
    auto pc = std::make_shared<rtc::PeerConnection>(c);
    pc->onLocalDescription([this](rtc::Description desc) {
        std::function<void(json)> done;
        json tracks = json::object();
        {
            std::lock_guard<std::mutex> g(mu_);
            done = std::move(answer_);
            answer_ = nullptr;
            for (auto& kv : track_verdict_) tracks[kv.first] = kv.second;
        }
        if (done) done({ {"sdp", std::string(desc)}, {"tracks", tracks} });
    });
    pc->onLocalCandidate([this](rtc::Candidate cand) {
        hooks_.push({ {"op", "rtc"}, {"candidate", cand.candidate()}, {"mid", cand.mid()} });
    });
    pc->onTrack([this](std::shared_ptr<rtc::Track> t) { on_track(std::move(t)); });
    pc->onStateChange([this](rtc::PeerConnection::State s) {
        const char* name = s == rtc::PeerConnection::State::Connected ? "connected"
                         : s == rtc::PeerConnection::State::Connecting ? "connecting"
                         : s == rtc::PeerConnection::State::Disconnected ? "disconnected"
                         : s == rtc::PeerConnection::State::Failed ? "failed"
                         : s == rtc::PeerConnection::State::Closed ? "closed" : "new";
        up_ = s == rtc::PeerConnection::State::Connected;
        hooks_.push({ {"op", "event"}, {"event", "rtc"}, {"state", name}, {"text", std::string("webrtc ") + name} });
        if (!up_) return;
        std::map<uint16_t, bool> wanted;
        { std::lock_guard<std::mutex> g(mu_); wanted = wanted_; }
        for (auto& kv : wanted) add_channel(kv.first, kv.second);
        hooks_.writable();
    });
    {
        std::lock_guard<std::mutex> g(mu_);
        pc_ = pc;
        wanted_[0] = true;           /* the anchor: pings and pongs */
    }
    json urls = json::array();
    for (const auto& s : cfg_.ice) {
        json e = { {"urls", s.urls} };
        if (!s.username.empty()) { e["username"] = s.username; e["credential"] = s.credential; }
        urls.push_back(e);
    }
    return { {"ice_servers", urls} };
}

void Rtc::offer(const std::string& sdp, const json& tracks, std::function<void(json)> done) {
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard<std::mutex> g(mu_);
        pc = pc_;
        track_req_.clear(); track_verdict_.clear();
        if (tracks.is_object()) for (auto& kv : tracks.items()) track_req_[kv.key()] = kv.value();
        answer_ = std::move(done);
    }
    pc->setRemoteDescription(rtc::Description(sdp, "offer"));
    pc->setLocalDescription();
}

void Rtc::candidate(const std::string& cand, const std::string& mid) {
    std::shared_ptr<rtc::PeerConnection> pc;
    { std::lock_guard<std::mutex> g(mu_); pc = pc_; }
    if (pc) pc->addRemoteCandidate(rtc::Candidate(cand, mid));
}

void Rtc::add_channel(uint16_t id, bool reliable) {
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard<std::mutex> g(mu_);
        wanted_[id] = reliable;
        if (!up_ || channels_.count(id) || !pc_) return;
        pc = pc_;
    }
    rtc::DataChannelInit init;
    init.negotiated = true;
    init.id = id;
    if (!reliable) { init.reliability.unordered = true; init.reliability.maxRetransmits = 0; }
    std::shared_ptr<rtc::DataChannel> dc;
    try { dc = pc->createDataChannel("d" + std::to_string(id), init); }
    catch (const std::exception& e) {
        hooks_.push({ {"op", "event"}, {"event", "error"}, {"text", std::string("data channel ") + std::to_string(id) + ": " + e.what()} });
        return;
    }
    dc->setBufferedAmountLowThreshold(kLowWater);
    dc->onBufferedAmountLow([this] { hooks_.writable(); });
    dc->onMessage([this](rtc::binary b) { hooks_.frame(reinterpret_cast<const uint8_t*>(b.data()), b.size()); },
                  [](rtc::string) {});
    dc->onClosed([this, id] { std::lock_guard<std::mutex> g(mu_); channels_.erase(id); });
    std::lock_guard<std::mutex> g(mu_);
    channels_[id] = dc;
}

void Rtc::remove_channel(uint16_t id) {
    std::shared_ptr<rtc::DataChannel> dc;
    {
        std::lock_guard<std::mutex> g(mu_);
        wanted_.erase(id);
        bindings_.erase(id);
        auto it = channels_.find(id);
        if (it == channels_.end()) return;
        dc = it->second;
        channels_.erase(it);
    }
    dc->resetCallbacks();
    dc->close();
}

bool Rtc::has(uint16_t id) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = channels_.find(id);
    return it != channels_.end() && it->second->isOpen();
}

bool Rtc::send(uint16_t id, const std::string& frame, size_t limit) {
    std::shared_ptr<rtc::DataChannel> dc;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = channels_.find(id);
        if (it == channels_.end()) return false;
        dc = it->second;
    }
    if (!up_ || !dc->isOpen() || frame.size() > dc->maxMessageSize() || dc->bufferedAmount() > limit) return false;
    try { return dc->send(reinterpret_cast<const std::byte*>(frame.data()), frame.size()); }
    catch (const std::exception&) { return false; }
}

/* ---- video tracks ------------------------------------------------------------------ */

void Rtc::on_track(std::shared_ptr<rtc::Track> track) {
    std::string mid = track->mid();
    json req;
    { std::lock_guard<std::mutex> g(mu_); auto it = track_req_.find(mid); if (it != track_req_.end()) req = it->second; }
    auto refuse = [&](const std::string& why) {
        rtc::Description::Media desc = track->description();
        desc.markRemoved();
        track->setDescription(desc);
        std::lock_guard<std::mutex> g(mu_);
        track_verdict_[mid] = why;
    };
    if (!req.is_object()) { refuse("no request for this line"); return; }
    uint16_t id = (uint16_t)req.value("id", 0);
    const Codec* c = hooks_.codec(id);
    if (!c || !c->typed()) { refuse("no such typed entity"); return; }

    auto b = std::make_shared<Binding>();
    b->mid = mid; b->path = req.value("path", std::string()); b->keep_data = req.value("keep_data", false);
    std::string prefix;
    if (b->path.empty()) {
        if (c->schema().name() != "VideoFrame") { refuse("the value is not a VideoFrame"); return; }
    } else {
        int idx = c->schema().field_index(b->path);
        if (idx < 0 || c->rows()[(size_t)idx].type_name != "VideoFrame") { refuse("'" + b->path + "' is not a VideoFrame"); return; }
        prefix = b->path + ".";
    }
    b->f_codec = prefix + "codec"; b->f_key = prefix + "keyframe"; b->f_pts = prefix + "pts"; b->f_data = prefix + "data";
    b->data_field = c->schema().field_index(b->f_data);
    if (b->data_field < 0) { refuse("VideoFrame fields not found"); return; }

    std::string cname = "rant-" + mid;
    uint32_t ssrc = 1000u + id;
    rtc::Description::Media desc = track->description();
    desc.addSSRC(ssrc, cname, "rant", mid);
    track->setDescription(desc);
    bool h264_mode1 = false;
    for (int p : desc.payloadTypes()) {
        const rtc::Description::Media::RtpMap* m = desc.rtpMap(p);
        if (!m || p > 127) continue;
        int codec = m->format == "H264" ? 2 : m->format == "H265" ? 3 : m->format == "AV1" ? 4 : 0;
        if (!codec || (codec == 2 && h264_mode1)) continue;
        bool mode1 = false;
        for (const std::string& f : m->fmtps) if (f.find("packetization-mode=1") != std::string::npos) mode1 = true;
        if (!b->pt[codec] || (codec == 2 && mode1)) { b->pt[codec] = (uint8_t)p; if (codec == 2) h264_mode1 = mode1; }
    }
    b->track = track;
    b->rtp = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, cname, b->pt[2] ? b->pt[2] : 96, 90000);
    std::lock_guard<std::mutex> g(mu_);
    bindings_[id] = b;
    track_verdict_[mid] = "ok";
}

static bool starts_with_startcode(const uint8_t* p, size_t n) {
    return n >= 3 && p[0] == 0 && p[1] == 0 && (p[2] == 1 || (n >= 4 && p[2] == 0 && p[3] == 1));
}

/* the keyframe flag, or an IDR NAL in the bitstream when the publisher left the flag unset */
static bool is_keyframe(int codec, const uint8_t* p, size_t n, bool flag) {
    if (flag || codec == 4) return flag;
    for (size_t i = 0; i + 3 < n; i++) {
        if (p[i] != 0 || p[i + 1] != 0) continue;
        size_t h = p[i + 2] == 1 ? i + 3 : (p[i + 2] == 0 && i + 4 < n && p[i + 3] == 1) ? i + 4 : 0;
        if (!h || h >= n) continue;
        int t = codec == 2 ? (p[h] & 0x1f) : ((p[h] >> 1) & 0x3f);
        if (codec == 2 ? t == 5 : (t == 19 || t == 20)) return true;
    }
    return false;
}

int Rtc::route_video(uint16_t id, rant::Bytes msg, const Codec& codec, uint64_t written_us, uint64_t capture_us) {
    std::shared_ptr<Binding> b;
    { std::lock_guard<std::mutex> g(mu_); auto it = bindings_.find(id); if (it != bindings_.end()) b = it->second; }
    if (!b || !up_ || !b->track || !b->track->isOpen()) return -1;
    d::RantBytes m = d::rant_bytes(msg.data(), msg.size());
    const d::RantSchema* s = codec.schema().raw();
    int vcodec = (int)d::rant_get_uint(m, s, b->f_codec.c_str());
    bool flag = d::rant_get_uint(m, s, b->f_key.c_str()) != 0;
    int64_t pts = d::rant_get_int(m, s, b->f_pts.c_str());
    d::RantBytes bs = d::rant_get_array(m, s, b->f_data.c_str());
    if (!bs.data || !bs.len || vcodec < 2 || vcodec > 4 || !b->pt[vcodec]) return -1;

    int sep = vcodec == 4 ? 0 : (starts_with_startcode(bs.data, bs.len) ? 1 : 2);
    if (vcodec != b->codec || sep != b->sep) {
        auto nsep = sep == 1 ? rtc::NalUnit::Separator::StartSequence : rtc::NalUnit::Separator::Length;
        std::shared_ptr<rtc::MediaHandler> pk;
        b->rtp->payloadType = b->pt[vcodec];
        size_t frag = cfg_.mtu - 12 - 8 - 40 - 10;      /* RTP, UDP, IPv6 and the SRTP tag */
        if (vcodec == 2) pk = std::make_shared<rtc::H264RtpPacketizer>(nsep, b->rtp, frag);
        else if (vcodec == 3) pk = std::make_shared<rtc::H265RtpPacketizer>(nsep, b->rtp, frag);
        else pk = std::make_shared<rtc::AV1RtpPacketizer>(rtc::AV1RtpPacketizer::Packetization::TemporalUnit, b->rtp, frag);
        pk->addToChain(std::make_shared<rtc::RtcpSrReporter>(b->rtp));
        pk->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        b->track->setMediaHandler(pk);
        b->codec = vcodec; b->sep = sep; b->keyed = false;
    }
    if (!b->keyed) {
        if (!is_keyframe(vcodec, bs.data, bs.len, flag)) return b->data_field;   /* a decoder starts on a keyframe */
        b->keyed = true;
    }
    /* the capture stamp lets a page match overlays to the frame on screen */
    uint64_t us = capture_us ? capture_us : pts > 0 ? (uint64_t)pts : written_us;
    b->rtp->timestamp = (uint32_t)(us * 9 / 100);
    try { b->track->send(reinterpret_cast<const std::byte*>(bs.data), bs.len); }
    catch (const std::exception&) { return -1; }
    return b->keep_data ? -1 : b->data_field;
}

}
