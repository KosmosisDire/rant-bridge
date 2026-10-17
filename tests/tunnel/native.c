/* Rant's native client on a freshly built bridge: a node on the bridge's sockets and a node of
 * this host on loopback hear each other both ways, again after the bridge restarts, and a
 * wrong passphrase is refused with its reason. native.mjs runs it.
 *   tunnel_native <ws url> <passphrase> <domain> */
#define RANT_IMPLEMENTATION
#include "rant.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int got_ping, got_back;
static int failed;

static void check(int ok, const char *what){
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    if (!ok) failed++;
}

static void on_bridged(const RantMsg *m){ if (m->data.len == 4 && memcmp(m->data.data, "ping", 4) == 0) got_ping++; }
static void on_local(const RantMsg *m){ if (m->data.len == 4 && memcmp(m->data.data, "back", 4) == 0) got_back++; }

static void sleep_ms(int ms){
    RantTimestamp end = rant_timestamp_now() + (RantTimestamp)ms * 1000;
    while (rant_timestamp_now() < end) {}
}

/* Sends both ways until each side heard five, or ten seconds pass. */
static int exchange(RantTopic *ping, RantTopic *back){
    int i;
    got_ping = got_back = 0;
    for (i = 0; i < 100 && (got_ping < 5 || got_back < 5); i++){
        rant_topic_send(ping, rant_bytes("ping", 4), NULL);
        rant_topic_send(back, rant_bytes("back", 4), NULL);
        sleep_ms(100);
    }
    printf("  heard %d and %d after %d rounds\n", got_ping, got_back, i);
    return got_ping >= 5 && got_back >= 5;
}

int main(int argc, char **argv){
    RantAllocator a1, a2, a3;
    RantNodeOpts o;
    RantTopicOpts reliable;
    RantNode *bridged, *local;
    RantTopic *ping, *back;
    RantEvent le;
    char line[16];
    if (argc < 4) return 2;

    memset(&o, 0, sizeof o);
    o.domain = (uint16_t)atoi(argv[3]);
    o.net.multicast_interface = "127.0.0.1";       /* the bridge's loopback, and this host's */
    o.discovery.announce_interval_us = 300000;
    memset(&reliable, 0, sizeof reliable);
    reliable.qos.reliability = RANT_RELIABLE;

    a1 = rant_allocator_heap(0);
    o.net.bridge = argv[1]; o.net.bridge_passphrase = argv[2];
    bridged = rant_node_open(&a1, "native-bridged", on_bridged, NULL, &o);
    le = rant_last_error(NULL);
    check(bridged != NULL, "a node opens on the bridge's sockets");
    if (!bridged){ printf("  why: %s\n", le.detail ? le.detail : "?"); return 1; }
    a2 = rant_allocator_heap(0);
    o.net.bridge = ""; o.net.bridge_passphrase = NULL;
    local = rant_node_open(&a2, "native-local", on_local, NULL, &o);
    check(local != NULL, "a node of this host opens beside it");
    if (!local) return 1;

    ping = rant_node_create_topic(local, "tunnel/ping", RANT_PUB_ONLY, NULL, &reliable);
    back = rant_node_create_topic(bridged, "tunnel/back", RANT_PUB_ONLY, NULL, &reliable);
    check(ping && back && rant_node_create_topic(bridged, "tunnel/ping", RANT_SUB_ONLY, NULL, &reliable)
          && rant_node_create_topic(local, "tunnel/back", RANT_SUB_ONLY, NULL, &reliable), "topics on both");
    rant_node_start(bridged);
    rant_node_start(local);
    check(exchange(ping, back), "each hears the other");

    /* the harness restarts the bridge, which loses the session and its sockets */
    printf("restart now\n"); fflush(stdout);
    if (!fgets(line, sizeof line, stdin)) return 1;
    check(exchange(ping, back), "each hears the other again once the bridge is back");
    rant_node_close(bridged, 1);
    rant_node_close(local, 1);

    a3 = rant_allocator_heap(0);
    o.net.bridge = argv[1]; o.net.bridge_passphrase = "not it";
    check(rant_node_open(&a3, "native-refused", NULL, NULL, &o) == NULL, "a wrong passphrase opens nothing");
    le = rant_last_error(NULL);
    check(le.error == RANT_E_BRIDGE && le.detail && strstr(le.detail, "passphrase"), "and says why");
    return failed ? 1 : 0;
}
