#include "check.hpp"

#include "mdns.hpp"

#include <cstring>

using namespace bridge;

int main() {
    /* a query and an answer survive a build and a parse */
    mdns::Message q;
    q.questions.push_back({ mdns::kService, mdns::PTR, true });
    std::string qb = mdns::build(q);
    mdns::Message q2;
    CHECK(mdns::parse((const uint8_t*)qb.data(), qb.size(), q2));
    CHECK(!q2.response && q2.questions.size() == 1 && q2.questions[0].name == mdns::kService &&
          q2.questions[0].type == mdns::PTR && q2.questions[0].unicast);

    mdns::Message r;
    r.response = true;
    mdns::Record ptr; ptr.name = mdns::kService; ptr.type = mdns::PTR; ptr.target = "karbon._rant-bridge._tcp.local";
    mdns::Record srv; srv.name = ptr.target; srv.type = mdns::SRV; srv.port = 7480; srv.target = "rant-bridge-karbon.local"; srv.flush = true;
    mdns::Record txt; txt.name = ptr.target; txt.type = mdns::TXT; txt.txt = { "v=0.0.7", "name=karbon" };
    mdns::Record a; a.name = srv.target; a.type = mdns::A; a.ip = 0x0701a8c0;      /* 192.168.1.7 */
    r.answers = { ptr };
    r.additional = { srv, txt, a };
    std::string rb = mdns::build(r);
    mdns::Message r2;
    CHECK(mdns::parse((const uint8_t*)rb.data(), rb.size(), r2));
    CHECK(r2.response && r2.answers.size() == 1 && r2.additional.size() == 3);
    CHECK(r2.answers[0].target == ptr.target);
    CHECK(r2.additional[0].port == 7480 && r2.additional[0].target == srv.target && r2.additional[0].flush);
    CHECK(r2.additional[1].txt.size() == 2 && r2.additional[1].txt[1] == "name=karbon");
    CHECK(r2.additional[2].ip == a.ip);

    /* a compressed name, as other responders send them: the PTR target points back at the question */
    const uint8_t packed[] = {
        0, 0, 0x84, 0, 0, 1, 0, 1, 0, 0, 0, 0,
        12, '_', 'r', 'a', 'n', 't', '-', 'b', 'r', 'i', 'd', 'g', 'e', 4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0,
        0, 12, 0, 1,
        0xc0, 12, 0, 12, 0, 1, 0, 0, 0, 120, 0, 5, 2, 'p', 'c', 0xc0, 12,
    };
    mdns::Message c;
    CHECK(mdns::parse(packed, sizeof packed, c));
    CHECK(c.answers.size() == 1 && c.answers[0].name == mdns::kService && c.answers[0].target == "pc._rant-bridge._tcp.local");

    /* a pointer loop and a cut message are refused, never read past the end */
    uint8_t loop[] = { 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xc0, 12, 0, 1, 0, 1 };
    mdns::Message l;
    CHECK(!mdns::parse(loop, sizeof loop, l));
    CHECK(!mdns::parse((const uint8_t*)rb.data(), rb.size() - 3, r2));

    CHECK(mdns::same_name("Rant-Bridges.LOCAL", mdns::kSharedName));
    return finish();
}
