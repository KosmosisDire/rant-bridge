#include "check.hpp"

#include "codec.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

using namespace bridge;

static rant::Node test_node() {
    rant::NodeOptions o;
    o.domain = 150;                        /* never the lab mesh on domain 0 */
    o.multicast_interface = "127.0.0.1";
    o.disable_logs = true;
    o.disable_meta = true;
    return rant::Node("codec-test", o);
}

/* CBOR out and back must give the same message bytes. */
static void round_trip(const Codec& c, rant::Bytes msg) {
    cbor::Writer w;
    CHECK(c.encode(msg, w));
    cbor::Item v; std::string err;
    CHECK(cbor::parse(w.out, v, err));
    std::vector<uint8_t> back;
    bool ok = c.decode(v, back, err);
    if (!ok) std::fprintf(stderr, "decode: %s\n", err.c_str());
    CHECK(ok);
    CHECK(back.size() == msg.size() && std::memcmp(back.data(), msg.data(), msg.size()) == 0);
}

int main() {
    rant::Node node = test_node();

    {   /* scalars, a nested struct, 64 bit values and the IEEE specials */
        Codec c(node.schema("Twist3 { stamp: u64, delta: i64, linear: { x: f64, y: f64, z: f64 }, ok: bool, small: i8 }"));
        rant::MessageBuilder b(c.schema());
        b.set_uint("stamp", 18000000000000000123ull).set_int("delta", -5).set_f64("linear.x", 0.5)
         .set_f64("linear.y", std::numeric_limits<double>::quiet_NaN()).set_f64("linear.z", -INFINITY)
         .set_bool("ok", true).set_int("small", -3);
        CHECK(b.ok());
        round_trip(c, b.bytes());
        /* 64 bit fields always take the 8 byte form, so a client reads a bigint every time */
        cbor::Writer w;
        c.encode(b.bytes(), w);
        cbor::Item v; std::string err;
        cbor::parse(w.out, v, err);
        CHECK(v.type == cbor::Item::Map && v.items[0].s == "stamp" && v.items[1].u == 18000000000000000123ull);
    }
    {   /* struct arrays, strings, enums, typed arrays and a map */
        Codec c(node.schema("Scan { points: Float3[], corners: Float2[2], name: string<16>, label: string, "
                            "tags: string<8>[], mode: enum<u8> { Idle, Run }, samples: f32[], raw: u8[], extra: map }"));
        rant::MessageBuilder b(c.schema());
        b.set_array_count("points", 2).set_f32("points[1].z", 3.5f).set_f32("corners[1].x", 2.0f)
         .set_string("name", "front").set_string("label", "a longer variable string")
         .set_array_count("tags", 2).set_string_at("tags", 1, "hot").set_enum("mode", std::string_view("Run"));
        float samples[3] = { 1.0f, 2.0f, 3.0f };
        uint8_t raw[4] = { 1, 2, 3, 4 };
        b.set_array("samples", rant::Bytes(samples, sizeof samples)).set_array("raw", rant::Bytes(raw, sizeof raw));
        rant::MapWriter m;
        m.put_uint("count", 3).open_array("list").put_f64(nullptr, 1.5).close().put_string("who", "robot");   /* sorted keys: a decoded map comes back in key order */
        b.set_map("extra", m);
        CHECK(b.ok());
        round_trip(c, b.bytes());
        CHECK(c.path(c.schema().field_index("points[0].z")) == "points[].z");
    }
    {   /* a bare type is the value itself */
        Codec c(node.schema("f32[]"));
        rant::MessageBuilder b(c.schema());
        float v[2] = { 1.0f, 2.0f };
        b.set_array("", rant::Bytes(v, sizeof v));
        round_trip(c, b.bytes());
    }
    {   /* refusals name the field */
        Codec c(node.schema("P { name: string<4>, n: u8 }"));
        cbor::Writer w;
        w.map(1); w.text("name"); w.text("far too long");
        cbor::Item v; std::string err;
        cbor::parse(w.out, v, err);
        std::vector<uint8_t> out;
        CHECK(!c.decode(v, out, err));
        CHECK(err.rfind("name:", 0) == 0);
        cbor::Writer w2;
        w2.map(1); w2.text("nope"); w2.uint(1);
        cbor::parse(w2.out, v, err);
        CHECK(!c.decode(v, out, err));
        CHECK(err == "nope: no such field");
    }
    return finish();
}
