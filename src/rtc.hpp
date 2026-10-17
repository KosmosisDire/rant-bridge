#pragma once

/* One session's WebRTC link: a data channel per entity (negotiated, id = entity id), the
 * anchor channel 0 for pings, and video tracks carrying a VideoFrame field of an entity. */

#include "codec.hpp"
#include "config.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace rtc { class PeerConnection; class DataChannel; class Track; class RtpPacketizationConfig; }

namespace bridge {

class Rtc {
public:
    struct Hooks {
        std::function<void(const nlohmann::json&)> push;          /* a JSON push to the client */
        std::function<void(const uint8_t*, size_t)> frame;        /* a frame from any channel */
        std::function<void()> writable;                            /* a channel drained */
        std::function<const Codec*(uint16_t id)> codec;           /* an entity's value codec, for tracks */
    };
    Rtc(const Config& cfg, Hooks hooks);
    ~Rtc();

    /* The reply to the bare rtc op: the ICE servers the client should use too. */
    nlohmann::json start();
    /* Applies the client's offer and answers through done, with a verdict per video line. */
    void offer(const std::string& sdp, const nlohmann::json& tracks, std::function<void(nlohmann::json)> done);
    void candidate(const std::string& cand, const std::string& mid);

    void add_channel(uint16_t id, bool reliable);
    void remove_channel(uint16_t id);
    /* false when no channel is open for id, the frame is too big for it, or more than limit
     * bytes wait in it */
    bool send(uint16_t id, const std::string& frame, size_t limit = 64u << 10);
    /* a channel for id is open */
    bool has(uint16_t id);
    bool up() const { return up_; }

    /* Sends the VideoFrame at the entity's bound track field. The field to empty in the
     * forwarded value, or -1 when the frame path must carry the pixels. */
    int route_video(uint16_t id, rant::Bytes msg, const Codec& codec, uint64_t written_us, uint64_t capture_us);

private:
    struct Binding;
    void on_track(std::shared_ptr<rtc::Track> track);

    const Config& cfg_;
    Hooks hooks_;
    std::mutex mu_;
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::map<uint16_t, std::shared_ptr<rtc::DataChannel>> channels_;
    std::map<uint16_t, bool> wanted_;                    /* channels asked for before the link was up */
    std::map<std::string, nlohmann::json> track_req_;    /* by offered mid */
    std::map<std::string, std::string> track_verdict_;
    std::map<uint16_t, std::shared_ptr<Binding>> bindings_;
    std::function<void(nlohmann::json)> answer_;
    std::atomic<bool> up_{ false };
};

}
