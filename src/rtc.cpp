#include "rtc.hpp"

#include <rtc/rtc.hpp>

#include <cstdio>

namespace bridge {

using json = nlohmann::json;

Rtc::Rtc(const Config& cfg, Hooks hooks) : cfg_(cfg), hooks_(std::move(hooks)) {}

Rtc::~Rtc() {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::map<uint16_t, std::shared_ptr<rtc::DataChannel>> chans;
    {
        std::lock_guard<std::mutex> g(mu_);
        pc = std::move(pc_);
        chans = std::move(channels_);
    }
    up_ = false;
    for (auto& kv : chans) { kv.second->resetCallbacks(); kv.second->close(); }
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
    if (cfg_.rtc_port_lo) { c.portRangeBegin = cfg_.rtc_port_lo; c.portRangeEnd = cfg_.rtc_port_hi; }
    c.mtu = cfg_.mtu;
    auto pc = std::make_shared<rtc::PeerConnection>(c);
    pc->onLocalDescription([this](rtc::Description desc) {
        std::function<void(json)> done;
        {
            std::lock_guard<std::mutex> g(mu_);
            done = std::move(answer_);
            answer_ = nullptr;
        }
        if (done) done({ {"sdp", std::string(desc)} });
    });
    pc->onLocalCandidate([this](rtc::Candidate cand) {
        hooks_.push({ {"op", "rtc"}, {"candidate", cand.candidate()}, {"mid", cand.mid()} });
    });
    pc->onStateChange([this](rtc::PeerConnection::State s) {
        const char* name = s == rtc::PeerConnection::State::Connected ? "connected"
                         : s == rtc::PeerConnection::State::Connecting ? "connecting"
                         : s == rtc::PeerConnection::State::Disconnected ? "disconnected"
                         : s == rtc::PeerConnection::State::Failed ? "failed"
                         : s == rtc::PeerConnection::State::Closed ? "closed" : "new";
        up_ = s == rtc::PeerConnection::State::Connected;
        hooks_.push({ {"op", "event"}, {"event", "rtc"}, {"state", name}, {"text", std::string("webrtc ") + name} });
        if (!up_) return;
        open_channel(kPingChannel, true);
        open_channel(kTunnelChannel, false);
    });
    {
        std::lock_guard<std::mutex> g(mu_);
        pc_ = pc;
    }
    json urls = json::array();
    for (const auto& s : cfg_.ice) {
        json e = { {"urls", s.urls} };
        if (!s.username.empty()) { e["username"] = s.username; e["credential"] = s.credential; }
        urls.push_back(e);
    }
    return { {"ice_servers", urls} };
}

void Rtc::offer(const std::string& sdp, std::function<void(json)> done) {
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard<std::mutex> g(mu_);
        pc = pc_;
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

void Rtc::open_channel(uint16_t id, bool reliable) {
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (channels_.count(id) || !pc_) return;
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
    dc->onMessage([this](rtc::binary b) { hooks_.message(reinterpret_cast<const uint8_t*>(b.data()), b.size()); },
                  [](rtc::string) {});
    dc->onClosed([this, id] { std::lock_guard<std::mutex> g(mu_); channels_.erase(id); });
    std::lock_guard<std::mutex> g(mu_);
    channels_[id] = dc;
}

bool Rtc::has(uint16_t id) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = channels_.find(id);
    return it != channels_.end() && it->second->isOpen();
}

bool Rtc::send(uint16_t id, const std::string& msg, size_t limit) {
    std::shared_ptr<rtc::DataChannel> dc;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = channels_.find(id);
        if (it == channels_.end()) return false;
        dc = it->second;
    }
    if (!up_ || !dc->isOpen() || msg.size() > dc->maxMessageSize() || dc->bufferedAmount() > limit) return false;
    try { return dc->send(reinterpret_cast<const std::byte*>(msg.data()), msg.size()); }
    catch (const std::exception&) { return false; }
}

}
