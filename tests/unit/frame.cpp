#include "check.hpp"

#include "frame.hpp"

#include <string>

using namespace bridge;

int main() {
    Frame f;
    f.op = OpResult; f.flags = 5; f.id = 7; f.seq = 42; f.peer = 3; f.written = 123456789; f.capture = 99;
    f.text = "cancelled"; f.payload = "abc";
    std::string b = build_frame(f);
    Frame g;
    CHECK(parse_frame((const uint8_t*)b.data(), b.size(), g));
    CHECK(g.op == OpResult && g.flags == 5 && g.id == 7 && g.seq == 42 && g.peer == 3);
    CHECK(g.written == 123456789 && g.capture == 99 && g.text == "cancelled" && g.payload == "abc" && !g.chunk);

    /* chunks arrive in any order and join into the message */
    std::string whole = "0123456789";
    Reassembly r;
    std::string out;
    for (uint32_t off : { 5u, 0u }) {
        Frame c; c.op = OpData; c.id = 2; c.chunk = true; c.msg = 9; c.total = 10; c.offset = off;
        c.payload = std::string_view(whole).substr(off, 5);
        std::string cb = build_frame(c);
        Frame p;
        CHECK(parse_frame((const uint8_t*)cb.data(), cb.size(), p));
        bool done = r.add(p, out, 1 << 20);
        CHECK(done == (off == 0));
    }
    CHECK(out == whole);

    /* a frame shorter than its header is refused */
    CHECK(!parse_frame((const uint8_t*)b.data(), 10, g));
    return finish();
}
