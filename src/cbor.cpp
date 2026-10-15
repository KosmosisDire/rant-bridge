#include "cbor.hpp"

#include <cmath>
#include <cstring>

namespace bridge::cbor {

void Writer::head(uint8_t major, uint64_t v) {
    uint8_t m = (uint8_t)(major << 5);
    if (v < 24) { out += (char)(m | v); return; }
    int n = v < 0x100 ? 1 : v < 0x10000 ? 2 : v <= 0xffffffffu ? 4 : 8;
    out += (char)(m | (n == 1 ? 24 : n == 2 ? 25 : n == 4 ? 26 : 27));
    for (int i = n - 1; i >= 0; i--) out += (char)(v >> (8 * i));
}

void Writer::head64(uint8_t major, uint64_t v) {
    out += (char)((major << 5) | 27);
    for (int i = 7; i >= 0; i--) out += (char)(v >> (8 * i));
}

void Writer::sint(int64_t v) {
    if (v < 0) head(NInt, (uint64_t)(-1 - v)); else head(UInt, (uint64_t)v);
}

void Writer::sint64(int64_t v) {
    if (v < 0) head64(NInt, (uint64_t)(-1 - v)); else head64(UInt, (uint64_t)v);
}

void Writer::f32(float v) {
    uint32_t u; std::memcpy(&u, &v, 4);
    out.push_back(static_cast<char>(0xfau));
    for (int i = 3; i >= 0; i--) out += (char)(u >> (8 * i));
}

void Writer::f64(double v) {
    uint64_t u; std::memcpy(&u, &v, 8);
    out.push_back(static_cast<char>(0xfbu));
    for (int i = 7; i >= 0; i--) out += (char)(u >> (8 * i));
}

double Item::number() const {
    if (type == Float) return f;
    if (type == UInt) return (double)u;
    if (type == NInt) return -1.0 - (double)u;
    return 0;
}

int64_t Item::integer() const {
    if (type == UInt) return (int64_t)u;
    if (type == NInt) return -1 - (int64_t)u;
    if (type == Float) return (int64_t)f;
    return 0;
}

namespace {

struct Parser {
    const uint8_t* p;
    size_t n, i = 0;
    std::string err;

    bool need(size_t k) {
        if (n - i >= k) return true;
        err = "truncated";
        return false;
    }
    bool arg(uint8_t ai, uint64_t& v) {
        if (ai < 24) { v = ai; return true; }
        int len = ai == 24 ? 1 : ai == 25 ? 2 : ai == 26 ? 4 : ai == 27 ? 8 : 0;
        if (!len) { err = "indefinite lengths are not supported"; return false; }
        if (!need((size_t)len)) return false;
        v = 0;
        for (int k = 0; k < len; k++) v = (v << 8) | p[i++];
        return true;
    }
    bool item(Item& out, int depth) {
        if (depth > 64) { err = "nested too deep"; return false; }
        if (!need(1)) return false;
        uint8_t b = p[i++], major = b >> 5, ai = b & 31;
        if (major == Simple) {
            if (ai == 20 || ai == 21) { out.type = Item::Bool; out.b = ai == 21; return true; }
            if (ai == 22 || ai == 23) { out.type = Item::Null; return true; }
            if (ai == 25) {                              /* half float */
                if (!need(2)) return false;
                uint16_t h = (uint16_t)(p[i] << 8 | p[i + 1]); i += 2;
                int e = (h >> 10) & 31; double m = h & 1023;
                double v = e == 0 ? std::ldexp(m, -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(m + 1024, e - 25);
                out.type = Item::Float; out.f = (h & 0x8000) ? -v : v;
                return true;
            }
            if (ai == 26) {
                if (!need(4)) return false;
                uint32_t u = 0; for (int k = 0; k < 4; k++) u = (u << 8) | p[i++];
                float f; std::memcpy(&f, &u, 4);
                out.type = Item::Float; out.f = f;
                return true;
            }
            if (ai == 27) {
                if (!need(8)) return false;
                uint64_t u = 0; for (int k = 0; k < 8; k++) u = (u << 8) | p[i++];
                std::memcpy(&out.f, &u, 8);
                out.type = Item::Float;
                return true;
            }
            err = "unsupported simple value";
            return false;
        }
        uint64_t v;
        if (!arg(ai, v)) return false;
        switch (major) {
        case UInt: out.type = Item::UInt; out.u = v; return true;
        case NInt: out.type = Item::NInt; out.u = v; return true;
        case Bytes: case Text:
            if (!need((size_t)v)) return false;
            out.type = major == Bytes ? Item::Bytes : Item::Text;
            out.s = std::string_view((const char*)p + i, (size_t)v);
            i += (size_t)v;
            return true;
        case Array: case Map: {
            uint64_t count = major == Map ? v * 2 : v;
            if (count > n - i) { err = "count past the end"; return false; }   /* each item takes a byte at least */
            out.type = major == Map ? Item::Map : Item::Array;
            out.items.resize((size_t)count);
            for (auto& it : out.items) if (!item(it, depth + 1)) return false;
            return true;
        }
        case Tag:
            out.type = Item::Tag; out.u = v;
            out.items.resize(1);
            return item(out.items[0], depth + 1);
        }
        err = "bad major type";
        return false;
    }
};

}

bool parse(std::string_view data, Item& out, std::string& err) {
    Parser ps{ (const uint8_t*)data.data(), data.size() };
    if (!ps.item(out, 0)) { err = ps.err; return false; }
    if (ps.i != ps.n) { err = "trailing bytes after the value"; return false; }
    return true;
}

}
