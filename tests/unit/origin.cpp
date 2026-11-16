#include "check.hpp"

#include "origin.hpp"

using namespace bridge;

int main() {
    auto d = default_origins();
    CHECK(origin_allowed(d, "http://localhost:6732"));
    CHECK(origin_allowed(d, "http://127.0.0.1:5173"));
    CHECK(origin_allowed(d, "http://[::1]:8000"));
    CHECK(origin_allowed(d, "http://192.168.1.20:6732"));
    CHECK(origin_allowed(d, "http://10.0.0.5"));
    CHECK(origin_allowed(d, "http://172.20.1.1:80"));
    CHECK(origin_allowed(d, "http://cage-pc.local:6732"));
    CHECK(origin_allowed(d, "https://explore.rantlib.dev"));
    CHECK(origin_allowed(d, "https://EXPLORE.rantlib.dev"));

    CHECK(!origin_allowed(d, "https://evil.example"));
    CHECK(!origin_allowed(d, "http://explore.rantlib.dev"));       /* the scheme counts */
    CHECK(!origin_allowed(d, "https://explore.rantlib.dev.evil.example"));
    CHECK(!origin_allowed(d, "http://172.32.0.1"));
    CHECK(!origin_allowed(d, "http://192.168.1.20.evil.example"));
    CHECK(!origin_allowed(d, "http://localhost.evil.example"));
    CHECK(!origin_allowed(d, "null"));                             /* a file or a sandboxed frame */
    CHECK(!origin_allowed(d, "file://localhost"));

    CHECK(origin_allowed({ "*" }, "https://anything.example"));
    CHECK(origin_allowed({ "https://hmi.example/" }, "https://hmi.example"));
    CHECK(!origin_allowed({ "https://hmi.example" }, "http://localhost:1"));
    CHECK(!origin_allowed({}, "http://localhost:1"));
    return finish();
}
