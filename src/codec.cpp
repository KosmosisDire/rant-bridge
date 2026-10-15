#include "codec.hpp"

#include <cmath>
#include <cstring>

namespace bridge {

using FT = rant::FieldType;
namespace d = rant::detail;

static bool opens_level(const rant::Schema::Field& f) {
    return f.kind == FT::Struct || ((f.kind == FT::Array || f.kind == FT::VArray) && f.elem == FT::Struct);
}

Codec::Codec(const rant::Schema& s) : schema_(s) {
    if (schema_.empty()) return;
    rows_ = schema_.fields();
    kids_.resize(rows_.size());
    parent_.assign(rows_.size(), -1);
    /* rows come depth first: a row's parent is the nearest open level one depth up */
    std::vector<uint16_t> open;
    for (uint16_t i = 0; i < rows_.size(); i++) {
        const auto& f = rows_[i];
        while (!open.empty() && rows_[open.back()].depth >= f.depth) open.pop_back();
        if (open.empty()) roots_.push_back(i);
        else { kids_[open.back()].push_back(i); parent_[i] = open.back(); }
        if (opens_level(f)) open.push_back(i);
    }
}

std::string Codec::path(int row) const {
    std::string out;
    for (int i = row; i >= 0; i = parent_[(size_t)i]) {
        const auto& f = rows_[(size_t)i];
        std::string part(f.name);
        if (i != row && f.kind != FT::Struct) part += "[]";
        out = out.empty() ? part : part + "." + out;
    }
    return out;
}

/* ---- encode -------------------------------------------------------------------- */

struct Codec::Ctx {
    d::RantBytes msg;
    cbor::Writer& w;
    std::vector<uint32_t> elems;   /* one index per enclosing struct array */
    int skip;
};

static uint32_t typed_tag(FT e) {
    switch (e) {
    case FT::U16: return cbor::TagU16; case FT::U32: return cbor::TagU32; case FT::U64: return cbor::TagU64;
    case FT::I8:  return cbor::TagI8;  case FT::I16: return cbor::TagI16; case FT::I32: return cbor::TagI32;
    case FT::I64: return cbor::TagI64; case FT::F32: return cbor::TagF32; case FT::F64: return cbor::TagF64;
    default: return 0;
    }
}

static void write_scalar(cbor::Writer& w, FT k, const d::RantValue& v) {
    switch (k) {
    case FT::U64: w.head64(cbor::UInt, v.v.u); break;
    case FT::I64: w.sint64(v.v.i); break;
    case FT::U8: case FT::U16: case FT::U32: w.uint(v.v.u); break;
    case FT::I8: case FT::I16: case FT::I32: w.sint(v.v.i); break;
    case FT::F32: w.f32((float)v.v.f); break;
    case FT::F64: w.f64(v.v.f); break;
    case FT::Bool: w.boolean(v.v.u != 0); break;
    default: w.null(); break;
    }
}

static void write_map_item(cbor::Writer& w, const rant::MapItem& m);

static void write_map_dict(cbor::Writer& w, const rant::MapDict& dict) {
    w.map(dict.size());
    for (const auto& kv : dict) { w.text(kv.first); write_map_item(w, kv.second); }
}

static void write_map_item(cbor::Writer& w, const rant::MapItem& m) {
    if (m.is_bool()) w.boolean(m.as_bool());
    else if (m.is_uint()) w.uint(m.as_uint());
    else if (m.is_int()) w.sint(m.as_int());
    else if (m.is_double()) w.f64(m.as_f64());
    else if (m.is_string()) w.text(m.as_string());
    else if (m.is_array()) { w.array(m.as_array().size()); for (const auto& e : m.as_array()) write_map_item(w, e); }
    else if (m.is_map()) write_map_dict(w, m.as_map());
    else w.null();
}

bool Codec::encode_members(Ctx& c, const std::vector<uint16_t>& members) const {
    c.w.map(members.size());
    for (uint16_t k : members) {
        c.w.text(rows_[k].name);
        if (!encode_row(c, k)) return false;
    }
    return true;
}

bool Codec::encode_row(Ctx& c, uint16_t i) const {
    const auto& f = rows_[i];
    cbor::Writer& w = c.w;
    if (f.kind == FT::Struct) return encode_members(c, kids_[i]);
    if ((f.kind == FT::Array || f.kind == FT::VArray) && f.elem == FT::Struct) {
        uint32_t n = f.kind == FT::Array ? f.count : d::rant_array_count_at(c.msg, schema_.raw(), i);
        w.array(n);
        for (uint32_t k = 0; k < n; k++) {
            c.elems.push_back(k);
            bool ok = encode_members(c, kids_[i]);
            c.elems.pop_back();
            if (!ok) return false;
        }
        return true;
    }
    d::RantValue v;
    if (!d::rant_get_value_at(c.msg, schema_.raw(), i, c.elems.data(), (uint16_t)c.elems.size(), &v)) return false;
    switch (f.kind) {
    case FT::String: case FT::VString:
        w.text(std::string_view((const char*)v.bytes.data, v.bytes.len));
        return true;
    case FT::Enum: {
        rant::Schema::EnumVariant ev;
        for (uint16_t k = 0; schema_.enum_variant(i, k, ev); k++)
            if (ev.value == v.v.i) { w.text(ev.name); return true; }
        w.sint(v.v.i);    /* a value with no name stays a number */
        return true;
    }
    case FT::Map:
        write_map_dict(w, rant::MapReader(rant::Bytes(v.bytes.data, v.bytes.len)).to_map());
        return true;
    case FT::Array: case FT::VArray: {
        const uint8_t* p = v.bytes.data;
        if (f.elem == FT::String) {
            size_t slot = 2u + f.str_cap;
            uint32_t n = slot ? (uint32_t)(v.bytes.len / slot) : 0;
            w.array(n);
            for (uint32_t k = 0; k < n; k++) {
                const uint8_t* s = p + k * slot;
                uint16_t len = (uint16_t)(s[0] | (s[1] << 8));
                if (len > f.str_cap) len = f.str_cap;
                w.text(std::string_view((const char*)s + 2, len));
            }
            return true;
        }
        if (i == c.skip) { w.bytes(nullptr, 0); return true; }
        if (f.elem == FT::U8) { w.bytes(p, v.bytes.len); return true; }
        if (f.elem == FT::Bool) {
            w.array(v.bytes.len);
            for (size_t k = 0; k < v.bytes.len; k++) w.boolean(p[k] != 0);
            return true;
        }
        if (uint32_t tag = typed_tag(f.elem)) { w.tag(tag); w.bytes(p, v.bytes.len); return true; }
        /* enum elements and anything else travel as the raw element bytes */
        w.bytes(p, v.bytes.len);
        return true;
    }
    default:
        write_scalar(w, f.kind, v);
        return true;
    }
}

bool Codec::encode(rant::Bytes msg, cbor::Writer& w, int skip) const {
    Ctx c{ d::rant_bytes(msg.data(), msg.size()), w, {}, skip };
    if (roots_.size() == 1 && rows_[roots_[0]].name.empty()) return encode_row(c, roots_[0]);   /* a bare type */
    return encode_members(c, roots_);
}

/* ---- decode -------------------------------------------------------------------- */

static size_t elem_bytes(FT e) {
    switch (e) {
    case FT::U8: case FT::I8: case FT::Bool: return 1;
    case FT::U16: case FT::I16: return 2;
    case FT::U32: case FT::I32: case FT::F32: return 4;
    default: return 8;
    }
}

static void pack(std::string& out, FT e, const cbor::Item& n) {
    uint8_t b[8];
    switch (e) {
    case FT::F32: { float f = (float)n.number(); std::memcpy(b, &f, 4); break; }
    case FT::F64: { double f = n.number(); std::memcpy(b, &f, 8); break; }
    case FT::Bool: b[0] = n.type == cbor::Item::Bool ? n.b : n.number() != 0; break;
    default: { int64_t v = n.integer(); std::memcpy(b, &v, 8); break; }   /* little endian hosts only */
    }
    out.append((const char*)b, elem_bytes(e));
}

static bool to_map_writer(rant::MapWriter& w, const char* key, const cbor::Item& v, std::string& err) {
    switch (v.type) {
    case cbor::Item::Bool: w.put_bool(key, v.b); return true;
    case cbor::Item::UInt: w.put_uint(key, v.u); return true;
    case cbor::Item::NInt: w.put_int(key, v.integer()); return true;
    case cbor::Item::Float: w.put_f64(key, v.f); return true;
    case cbor::Item::Text: w.put_string(key, v.s); return true;
    case cbor::Item::Array:
        w.open_array(key);
        for (const auto& e : v.items) if (!to_map_writer(w, nullptr, e, err)) return false;
        w.close();
        return true;
    case cbor::Item::Map:
        w.open_map(key);
        for (size_t k = 0; k + 1 < v.items.size(); k += 2) {
            if (v.items[k].type != cbor::Item::Text) { err = "map keys must be text"; return false; }
            std::string sub(v.items[k].s);
            if (!to_map_writer(w, sub.c_str(), v.items[k + 1], err)) return false;
        }
        w.close();
        return true;
    default:
        err = "a map holds numbers, booleans, text, lists and maps only";
        return false;
    }
}

bool Codec::decode_members(rant::MessageBuilder& b, const cbor::Item& v, const std::vector<uint16_t>& members,
                           const std::string& prefix, std::string& err) const {
    if (v.type != cbor::Item::Map) { err = (prefix.empty() ? "the value" : prefix) + ": expected a map"; return false; }
    for (size_t k = 0; k + 1 < v.items.size(); k += 2) {
        const cbor::Item& key = v.items[k];
        if (key.type != cbor::Item::Text) { err = prefix + ": member names must be text"; return false; }
        int hit = -1;
        for (uint16_t m : members) if (rows_[m].name == key.s) { hit = m; break; }
        std::string path = prefix + std::string(key.s);
        if (hit < 0) { err = path + ": no such field"; return false; }
        if (!decode_row(b, v.items[k + 1], (uint16_t)hit, path, err)) return false;
    }
    return true;
}

bool Codec::decode_row(rant::MessageBuilder& b, const cbor::Item& v, uint16_t i, const std::string& path, std::string& err) const {
    const auto& f = rows_[i];
    const char* p = path.c_str();
    auto refused = [&](const char* why) { err = (path.empty() ? "the value" : path) + ": " + why; return false; };
    auto check = [&]() { return b.ok() ? true : refused("refused by the schema (out of range or too long)"); };

    if (f.kind == FT::Struct) return decode_members(b, v, kids_[i], path.empty() ? "" : path + ".", err);
    if (f.kind == FT::Array || f.kind == FT::VArray) {
        if (f.elem == FT::Struct || f.elem == FT::String) {
            if (v.type != cbor::Item::Array) return refused("expected a list");
            if (f.kind == FT::Array && v.items.size() > f.count) return refused("too many elements");
            if (f.kind == FT::VArray) { b.set_array_count(p, (uint32_t)v.items.size()); if (!check()) return false; }
            for (size_t k = 0; k < v.items.size(); k++) {
                std::string at = path + "[" + std::to_string(k) + "]";
                if (f.elem == FT::String) {
                    if (v.items[k].type != cbor::Item::Text) { err = at + ": expected text"; return false; }
                    b.set_string_at(p, (uint16_t)k, v.items[k].s);
                    if (!check()) return false;
                } else if (!decode_members(b, v.items[k], kids_[i], at + ".", err)) return false;
            }
            return true;
        }
        std::string packed;
        if (v.type == cbor::Item::Bytes) packed.assign(v.s);
        else if (v.type == cbor::Item::Tag && !v.items.empty() && v.items[0].type == cbor::Item::Bytes) {
            if (v.u != typed_tag(f.elem)) return refused("typed array of the wrong element type");
            packed.assign(v.items[0].s);
        } else if (v.type == cbor::Item::Array) {
            for (const auto& e : v.items) {
                if (!e.is_number() && e.type != cbor::Item::Bool) return refused("expected numbers");
                pack(packed, f.elem, e);
            }
        } else return refused("expected a list");
        b.set_array(p, rant::Bytes(packed.data(), packed.size()));
        return check();
    }
    switch (f.kind) {
    case FT::String: case FT::VString:
        if (v.type != cbor::Item::Text) return refused("expected text");
        b.set_string(p, v.s);
        return check();
    case FT::Enum:
        if (v.type == cbor::Item::Text) b.set_enum(p, v.s);
        else if (v.type == cbor::Item::UInt || v.type == cbor::Item::NInt) b.set_enum(p, v.integer());
        else return refused("expected an option name");
        return b.ok() ? true : refused("not an option of this enum");
    case FT::Map: {
        if (v.type != cbor::Item::Map) return refused("expected a map");
        for (size_t cap = 256 + v.items.size() * 64;; cap *= 4) {
            rant::MapWriter w(cap);
            std::string why;
            bool ok = true;
            for (size_t k = 0; ok && k + 1 < v.items.size(); k += 2) {
                if (v.items[k].type != cbor::Item::Text) return refused("map keys must be text");
                std::string key(v.items[k].s);
                ok = to_map_writer(w, key.c_str(), v.items[k + 1], why);
            }
            if (!why.empty()) return refused(why.c_str());
            if (w.ok()) { b.set_map(p, w.finish()); return check(); }
            if (cap > (64u << 20)) return refused("too large");
        }
    }
    case FT::Bool:
        if (v.type != cbor::Item::Bool && !v.is_number()) return refused("expected true or false");
        b.set_bool(p, v.type == cbor::Item::Bool ? v.b : v.number() != 0);
        return check();
    case FT::F32: case FT::F64:
        if (!v.is_number()) return refused("expected a number");
        if (f.kind == FT::F32) b.set_f32(p, (float)v.number()); else b.set_f64(p, v.number());
        return check();
    default: {
        if (!v.is_number()) return refused("expected a number");
        if (v.type == cbor::Item::Float && v.f != std::floor(v.f)) return refused("expected a whole number");
        bool is_unsigned = f.kind == FT::U8 || f.kind == FT::U16 || f.kind == FT::U32 || f.kind == FT::U64;
        if (is_unsigned) {
            if (v.type == cbor::Item::NInt || (v.type == cbor::Item::Float && v.f < 0)) return refused("expected a number of zero or more");
            b.set_uint(p, v.type == cbor::Item::UInt ? v.u : (uint64_t)v.f);
        } else b.set_int(p, v.integer());
        return check();
    }
    }
}

bool Codec::decode(const cbor::Item& v, std::vector<uint8_t>& out, std::string& err) const {
    rant::MessageBuilder b(schema_);
    bool ok = roots_.size() == 1 && rows_[roots_[0]].name.empty()
                  ? decode_row(b, v, roots_[0], "", err)
                  : decode_members(b, v, roots_, "", err);
    if (!ok) return false;
    rant::Bytes m = b.bytes();
    out.assign(m.data(), m.data() + m.size());
    return true;
}

}
