#pragma once

/* One session's WebRTC link: two negotiated data channels, 0 reliable and ordered for pings,
 * 1 unordered with no retransmits for batches of datagrams. */

#include "config.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace rtc { class PeerConnection; class DataChannel; }

namespace bridge {

constexpr uint16_t kPingChannel = 0;
constexpr uint16_t kTunnelChannel = 1;

class Rtc {
public:
    struct Hooks {
        std::function<void(const nlohmann::json&)> push;          /* a JSON push to the client */
        std::function<void(const uint8_t*, size_t)> message;      /* a binary message from a channel */
    };
    Rtc(const Config& cfg, Hooks hooks);
    ~Rtc();

    /* The reply to the bare rtc op: the ICE servers the client should use too. */
    nlohmann::json start();
    /* Applies the client's offer and answers through done. */
    void offer(const std::string& sdp, std::function<void(nlohmann::json)> done);
    void candidate(const std::string& cand, const std::string& mid);

    /* false when the channel is not open, the message is too big for it, or more than limit
     * bytes wait in it */
    bool send(uint16_t channel, const std::string& msg, size_t limit = 64u << 10);
    bool has(uint16_t channel);
    bool up() const { return up_; }

private:
    void open_channel(uint16_t id, bool reliable);

    const Config& cfg_;
    Hooks hooks_;
    std::mutex mu_;
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::map<uint16_t, std::shared_ptr<rtc::DataChannel>> channels_;
    std::function<void(nlohmann::json)> answer_;
    std::atomic<bool> up_{ false };
};

}
