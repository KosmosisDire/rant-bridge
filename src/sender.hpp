#pragma once

/* Everything a session sends toward its client as data frames: one lane per entity, sent a
 * chunk at a time in round robin, so a large message never holds up a small one for longer
 * than one chunk. A lane's bound follows Rant's queue rule: best effort drops the oldest. */

#include "frame.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace bridge {

/* One message waiting to go out, its header fields and its whole payload. */
struct Outgoing {
    uint8_t     op = 0, flags = 0;
    uint32_t    seq = 0, peer = 0;
    uint64_t    written = 0, capture = 0;
    std::string text, payload;
};

class Sender {
public:
    /* Sends one finished frame for entity id, false when its carrier is full right now. */
    using Carrier = std::function<bool(uint16_t id, const std::string& frame)>;
    /* Runs about every 25 ms on the sender thread, for pings and periodic checks. */
    using Tick = std::function<void()>;

    Sender(Carrier carrier, Tick tick, size_t chunk_bytes);
    ~Sender();

    void add_lane(uint16_t id, bool reliable, size_t cap_bytes);
    void remove_lane(uint16_t id);
    /* false when a reliable lane is over its bound: the client cannot keep up */
    bool push(uint16_t id, Outgoing m);
    /* Stops or resumes sending, as the client detaches and returns. Lanes keep filling meanwhile. */
    void set_attached(bool attached);
    /* Drops whatever waits on every lane (a fresh session start) */
    void clear();
    void wake();

    struct LaneStats { size_t bytes = 0, msgs = 0; uint64_t oldest_us = 0; uint32_t dropped = 0; };
    std::map<uint16_t, LaneStats> stats() const;

    /* Best effort messages dropped at a full lane, reported at most once a second per lane. */
    std::function<void(uint16_t id, uint32_t count)> on_drop;

private:
    struct Queued { Outgoing m; std::chrono::steady_clock::time_point at; };
    struct Lane {
        bool reliable = true;
        size_t cap = 0, bytes = 0, offset = 0;   /* offset: how much of the front message went out */
        uint16_t msg = 0;                        /* the front message's chunk number */
        std::deque<Queued> q;
        uint32_t dropped = 0, unreported = 0;
        std::chrono::steady_clock::time_point reported{};
    };
    void run();
    bool next_frame(uint16_t id, Lane& l, std::string& frame, bool& done);

    Carrier carrier_;
    Tick tick_;
    size_t chunk_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::map<uint16_t, Lane> lanes_;
    uint16_t cursor_ = 0;
    bool attached_ = false, stop_ = false, kicked_ = false;
    std::thread thread_;
};

}
