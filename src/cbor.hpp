#pragma once

/* The CBOR subset the bridge speaks with its clients (RFC 8949, typed arrays from RFC 8746).
 * PROTOCOL.md lists exactly what each Rant field kind becomes. */

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bridge::cbor {

enum Major : uint8_t { UInt = 0, NInt = 1, Bytes = 2, Text = 3, Array = 4, Map = 5, Tag = 6, Simple = 7 };

/* RFC 8746 little endian typed array tags, by element. u8 travels as a plain byte string. */
enum TypedTag : uint32_t {
    TagU16 = 69, TagU32 = 70, TagU64 = 71, TagI8 = 72, TagI16 = 77, TagI32 = 78, TagI64 = 79,
    TagF32 = 85, TagF64 = 86
};

class Writer {
public:
    std::string out;
    void head(uint8_t major, uint64_t v);
    void head64(uint8_t major, uint64_t v);    /* always the 8 byte form: a 64 bit field stays a bigint */
    void uint(uint64_t v) { head(UInt, v); }
    void sint(int64_t v);
    void sint64(int64_t v);
    void f32(float v);
    void f64(double v);
    void boolean(bool v) { out.push_back(static_cast<char>(v ? 0xf5u : 0xf4u)); }
    void null() { out.push_back(static_cast<char>(0xf6u)); }
    void bytes(const void* p, size_t n) { head(Bytes, n); out.append((const char*)p, n); }
    void text(std::string_view s) { head(Text, s.size()); out.append(s.data(), s.size()); }
    void array(size_t n) { head(Array, n); }
    void map(size_t n) { head(Map, n); }
    void tag(uint64_t t) { head(Tag, t); }
};

/* One parsed item. Bytes and text are views into the parsed input. */
struct Item {
    enum Type { UInt, NInt, Bytes, Text, Array, Map, Tag, Float, Bool, Null } type = Null;
    uint64_t u = 0;          /* the integer magnitude (NInt holds -1 - value), or the tag number */
    double   f = 0;
    bool     b = false;
    std::string_view s;      /* bytes or text */
    std::vector<Item> items; /* array elements, map keys and values interleaved, a tag's one child */

    bool is_number() const { return type == UInt || type == NInt || type == Float; }
    double number() const;
    int64_t integer() const;       /* UInt or NInt as signed, or an integral float */
};

/* Parses exactly one item spanning all of data. false with err on anything malformed. */
bool parse(std::string_view data, Item& out, std::string& err);

}
