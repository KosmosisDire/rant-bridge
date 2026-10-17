// The tunnel against a freshly built bridge, through Rant's own client TunnelNet: lent
// sockets, batched datagrams both ways, a group, and a lost session bound again. Everything
// stays on loopback.   node tests/tunnel/test.mjs <rant-bridge exe> <Rant web package dir>

import { spawn } from "node:child_process";
import dgram from "node:dgram";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

const [exe, pkg] = process.argv.slice(2);
const { TunnelNet } = await import(pathToFileURL(join(pkg, "dist/tunnel.js")).href);
const { naddr } = await import(pathToFileURL(join(pkg, "dist/net.js")).href);

const PORT = 17480 + (process.pid % 200);
const GROUP_PORT = 17700 + (process.pid % 200);
const lo = naddr("127.0.0.1");
const cfg = join(mkdtempSync(join(tmpdir(), "tunnel-test-")), "bridge.yaml");
writeFileSync(cfg, `port: ${PORT}\nbind: 127.0.0.1\npassphrase: tunnel-test\nwebrtc: false\ngrace_ms: 10000\n`);

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const until = async (f, ms) => { for (const end = Date.now() + ms; !f() && Date.now() < end;) await sleep(10); return f(); };
let failed = 0;
const check = (ok, what) => { console.log(`${ok ? "ok  " : "FAIL"} ${what}`); if (!ok) failed++; };

const start = () => spawn(exe, ["--config", cfg], { stdio: ["ignore", "ignore", "inherit"] });
let bridge = start();
const peer = dgram.createSocket("udp4");
let net = null;
try {
    await new Promise((r) => peer.bind(0, "127.0.0.1", r));
    const peerPort = peer.address().port;
    const heard = [];
    peer.on("message", (m, from) => heard.push({ text: m.toString(), port: from.port }));

    const events = [];
    for (let tries = 0; !net && tries < 50; tries++) {
        try {
            net = await TunnelNet.connect(`ws://127.0.0.1:${PORT}`, { passphrase: "tunnel-test", name: "tunnel-test",
                transport: "websocket", address: "127.0.0.1", onEvent: (e) => events.push(e) });
        } catch { await sleep(100); }
    }
    check(!!net, "a tunnel session opens");
    if (!net) throw new Error("no tunnel");

    const got = [];
    const sink = { deliver: (id, from, port, bytes) => got.push({ id, from, port, text: Buffer.from(bytes).toString() }) };
    const a = net.open(sink);
    const aPort = net.bind(a, lo, 0, false);
    check(aPort > 0, "a bind to port 0 answers a port at once");

    for (let i = 0; i < 50; i++) peer.send(`in ${i}`, aPort, "127.0.0.1");
    await until(() => got.length >= 50, 3000);
    check(got.length === 50 && got.every((g) => g.id === a && g.from === lo && g.port === peerPort),
          `datagrams to a lent socket reach the client with their source (${got.length} of 50)`);

    const batches = net.stats.sentBatches;
    for (let i = 0; i < 50; i++) net.send(a, new TextEncoder().encode(`out ${i}`), lo, peerPort);
    await until(() => heard.length >= 50, 3000);
    check(heard.length === 50 && heard.every((h, i) => h.text === `out ${i}` && h.port === aPort)
          && net.stats.sentBatches - batches === 1,
          `what one task sends leaves the lent socket as one batch, in order (${heard.length} of 50)`);

    got.length = 0;
    const g = net.open(sink);
    net.bind(g, net.loopbackGroupBind(lo), GROUP_PORT, true);
    net.membership(g, naddr("239.255.0.99"), lo, true);
    peer.setMulticastInterface("127.0.0.1");
    peer.setMulticastLoopback(true);
    await sleep(300);
    peer.send("group", GROUP_PORT, "239.255.0.99");
    await until(() => got.some((x) => x.id === g), 3000);
    check(got.some((x) => x.id === g && x.text === "group"), "a group joined on loopback delivers to the client");

    bridge.kill();
    await sleep(300);
    bridge = start();
    await until(() => events.some((e) => e.kind === "restored"), 8000);
    check(events.some((e) => e.kind === "restored"), "after the bridge restarts the session is set up again");
    got.length = 0;
    peer.send("again", aPort, "127.0.0.1");
    await until(() => got.length > 0, 3000);
    check(got.some((x) => x.id === a && x.text === "again"), "the lent port reaches the client again");
} catch (e) {
    console.log(`FAIL ${e?.message ?? e}`);
    failed++;
} finally {
    net?.shutdown();
    peer.close();
    bridge.kill();
}
process.exit(failed ? 1 : 0);
