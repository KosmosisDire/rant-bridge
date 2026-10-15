#include "sender.hpp"

#include <algorithm>
#include <vector>

namespace bridge {

using clock_type = std::chrono::steady_clock;

Sender::Sender(Carrier carrier, Tick tick, size_t chunk_bytes)
    : carrier_(std::move(carrier)), tick_(std::move(tick)), chunk_(chunk_bytes) {
    thread_ = std::thread([this] { run(); });
}

Sender::~Sender() {
    {
        std::lock_guard<std::mutex> g(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Sender::add_lane(uint16_t id, bool reliable, size_t cap_bytes) {
    std::lock_guard<std::mutex> g(mu_);
    Lane& l = lanes_[id];
    l = Lane{};
    l.reliable = reliable;
    l.cap = cap_bytes;
}

void Sender::remove_lane(uint16_t id) {
    std::lock_guard<std::mutex> g(mu_);
    lanes_.erase(id);
}

bool Sender::push(uint16_t id, Outgoing m) {
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = lanes_.find(id);
        if (it == lanes_.end()) return true;     /* the entity closed meanwhile */
        Lane& l = it->second;
        size_t size = m.payload.size() + m.text.size();
        if (l.reliable) {
            if (!l.q.empty() && l.bytes + size > l.cap) return false;
        } else {
            /* best effort keeps the newest: drop the oldest that is not half sent */
            size_t keep = l.offset ? 1 : 0;
            while (l.q.size() > keep && l.bytes + size > l.cap) {
                auto victim = l.q.begin() + (ptrdiff_t)keep;
                l.bytes -= victim->m.payload.size() + victim->m.text.size();
                l.q.erase(victim);
                l.dropped++; l.unreported++;
            }
        }
        l.bytes += size;
        l.q.push_back(Queued{ std::move(m), clock_type::now() });
        kicked_ = true;
    }
    cv_.notify_all();
    return true;
}

void Sender::set_attached(bool attached) {
    {
        std::lock_guard<std::mutex> g(mu_);
        attached_ = attached;
        kicked_ = true;
    }
    cv_.notify_all();
}

void Sender::clear() {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& kv : lanes_) { kv.second.q.clear(); kv.second.bytes = 0; kv.second.offset = 0; }
}

void Sender::wake() {
    {
        std::lock_guard<std::mutex> g(mu_);
        kicked_ = true;
    }
    cv_.notify_all();
}

std::map<uint16_t, Sender::LaneStats> Sender::stats() const {
    std::lock_guard<std::mutex> g(mu_);
    std::map<uint16_t, LaneStats> out;
    auto now = clock_type::now();
    for (const auto& kv : lanes_) {
        LaneStats s;
        s.bytes = kv.second.bytes; s.msgs = kv.second.q.size(); s.dropped = kv.second.dropped;
        if (!kv.second.q.empty())
            s.oldest_us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(now - kv.second.q.front().at).count();
        out[kv.first] = s;
    }
    return out;
}

bool Sender::next_frame(uint16_t id, Lane& l, std::string& frame, bool& done) {
    const Outgoing& m = l.q.front().m;
    size_t total = m.payload.size();
    Frame f;
    f.op = m.op; f.flags = m.flags; f.id = id; f.seq = m.seq; f.peer = m.peer;
    f.written = m.written; f.capture = m.capture; f.text = m.text;
    if (total <= chunk_ && l.offset == 0) {
        f.payload = m.payload;
        done = true;
    } else {
        size_t n = std::min(chunk_, total - l.offset);
        f.chunk = true; f.msg = l.msg; f.total = (uint32_t)total; f.offset = (uint32_t)l.offset;
        f.payload = std::string_view(m.payload).substr(l.offset, n);
        done = l.offset + n >= total;
    }
    frame = build_frame(f);
    return true;
}

void Sender::run() {
    auto last_tick = clock_type::now();
    std::unique_lock<std::mutex> lk(mu_);
    while (!stop_) {
        bool sent = false, blocked = false;
        if (attached_) {
            /* one round: each lane with work sends at most one frame, starting after the last sender */
            std::vector<uint16_t> order;
            for (auto& kv : lanes_) if (!kv.second.q.empty()) order.push_back(kv.first);
            std::rotate(order.begin(), std::upper_bound(order.begin(), order.end(), cursor_), order.end());
            for (uint16_t id : order) {
                auto it = lanes_.find(id);
                if (it == lanes_.end() || it->second.q.empty()) continue;
                std::string frame; bool done = false;
                next_frame(id, it->second, frame, done);
                size_t chunk_len = std::min(chunk_, it->second.q.front().m.payload.size() - it->second.offset);
                lk.unlock();
                bool ok = carrier_(id, frame);
                lk.lock();
                it = lanes_.find(id);
                if (!ok) { blocked = true; continue; }
                sent = true;
                cursor_ = id;
                if (it == lanes_.end() || it->second.q.empty()) continue;
                Lane& l = it->second;
                if (done) {
                    l.bytes -= l.q.front().m.payload.size() + l.q.front().m.text.size();
                    l.q.pop_front();
                    l.offset = 0;
                    l.msg++;
                } else l.offset += chunk_len;
            }
        }
        /* loss reports, at most once a second per lane, outside the lock */
        std::vector<std::pair<uint16_t, uint32_t>> drops;
        auto now = clock_type::now();
        for (auto& kv : lanes_) {
            Lane& l = kv.second;
            if (l.unreported && now - l.reported >= std::chrono::seconds(1)) {
                drops.emplace_back(kv.first, l.unreported);
                l.unreported = 0; l.reported = now;
            }
        }
        bool tick = now - last_tick >= std::chrono::milliseconds(25);
        if (!drops.empty() || tick) {
            lk.unlock();
            if (on_drop) for (auto& d : drops) on_drop(d.first, d.second);
            if (tick && tick_) tick_();
            lk.lock();
            if (tick) last_tick = now;
        }
        if (sent || stop_) continue;
        /* a full carrier has no wakeup of its own here, so look again soon */
        auto wait = blocked ? std::chrono::milliseconds(1) : std::chrono::milliseconds(25);
        cv_.wait_for(lk, wait, [this] { return kicked_ || stop_; });
        kicked_ = false;
    }
}

}
