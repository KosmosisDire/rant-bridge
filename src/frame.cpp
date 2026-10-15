#include "frame.hpp"

#include <cstring>

namespace bridge {

static void w16(std::string& o, uint16_t v) { o += (char)v; o += (char)(v >> 8); }
static void w32(std::string& o, uint32_t v) { for (int i = 0; i < 4; i++) o += (char)(v >> (8 * i)); }
static void w64(std::string& o, uint64_t v) { for (int i = 0; i < 8; i++) o += (char)(v >> (8 * i)); }
static uint16_t r16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t r32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t r64(const uint8_t* p) { return (uint64_t)r32(p) | ((uint64_t)r32(p + 4) << 32); }

std::string build_frame(const Frame& f) {
    size_t tl = f.text.size() > 255 ? 255 : f.text.size();
    std::string o;
    o.reserve(kHeader + tl + (f.chunk ? kChunkHeader : 0) + f.payload.size());
    o += (char)f.op;
    o += (char)(f.chunk ? (f.flags | kChunkFlag) : f.flags);
    w16(o, f.id); w32(o, f.seq); w32(o, f.peer); w64(o, f.written); w64(o, f.capture);
    o += (char)tl;
    o.append(f.text.data(), tl);
    if (f.chunk) { w16(o, f.msg); w32(o, f.total); w32(o, f.offset); }
    o.append(f.payload.data(), f.payload.size());
    return o;
}

bool parse_frame(const uint8_t* p, size_t n, Frame& f) {
    if (n < kHeader) return false;
    size_t tl = p[28], at = kHeader + tl;
    if (n < at) return false;
    f.op = p[0];
    f.chunk = (p[1] & kChunkFlag) != 0;
    f.flags = (uint8_t)(p[1] & ~kChunkFlag);
    f.id = r16(p + 2); f.seq = r32(p + 4); f.peer = r32(p + 8);
    f.written = r64(p + 12); f.capture = r64(p + 20);
    f.text = std::string_view((const char*)p + kHeader, tl);
    if (f.chunk) {
        if (n < at + kChunkHeader) return false;
        f.msg = r16(p + at); f.total = r32(p + at + 2); f.offset = r32(p + at + 6);
        at += kChunkHeader;
    }
    f.payload = std::string_view((const char*)p + at, n - at);
    return true;
}

bool Reassembly::add(const Frame& f, std::string& whole, uint64_t max_bytes) {
    if (!f.chunk) { whole.assign(f.payload); return true; }
    if (f.total > max_bytes || (uint64_t)f.offset + f.payload.size() > f.total) return false;
    Partial& part = parts_[f.id];
    if (part.total == 0 || part.msg != f.msg) {   /* a new message replaces an unfinished one */
        part = Partial{};
        part.msg = f.msg; part.total = f.total;
        part.bytes.assign(f.total, '\0');
    }
    std::memcpy(&part.bytes[f.offset], f.payload.data(), f.payload.size());
    part.got += (uint32_t)f.payload.size();
    if (part.got < part.total) return false;
    whole = std::move(part.bytes);
    parts_.erase(f.id);
    return true;
}

}
