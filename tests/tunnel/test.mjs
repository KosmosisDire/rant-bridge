// The tunnel against a freshly built bridge, through Rant's own client TunnelNet: lent
// sockets, batched datagrams both ways, a group, and a lost session bound again. Everything
// stays on loopback.   node tests/tunnel/test.mjs <rant-bridge exe> <Rant web package dir>

import { spawn } from "node:child_process";
import dgram from "node:dgram";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
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

/* a home of its own, or the bridge writes its state over that of the user's bridge */
const start = () => spawn(exe, ["--config", cfg], { env: { ...process.env, RANT_HOME: dirname(cfg) }, stdio: ["ignore", "ignore", "inherit"] });
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

    /* a whole node through the tunnel, against one on this host's own sockets */
    const { openNode, struct, f64, string } = await import(pathToFileURL(join(pkg, "dist/rant.js")).href);
    const domain = 150 + (process.pid % 10);
    const Reading = struct("Reading", { value: f64, where: string(16) });
    const page = await openNode("page", { net, domain, multicastInterface: "127.0.0.1" });
    const host = await openNode("host", { domain, multicastInterface: "127.0.0.1" });
    const seen = [];
    page.subscriber("reading", Reading, (v) => seen.push(v), { reliable: true });
    page.functionDefinition("Twice", (v) => ({ value: v.value * 2, where: "page" }), Reading, Reading);
    const pub = host.publisher("reading", Reading, { reliable: true });
    const twice = host.remoteFunction("Twice", Reading, Reading);
    await until(() => pub.matchCount === 1 && twice.matchCount === 1, 10000);
    pub.send({ value: 1.5, where: "host" });
    await until(() => seen.length > 0, 3000);
    check(seen[0]?.value === 1.5 && seen[0]?.where === "host", "a node through the tunnel hears a node on this host");
    const r = await twice.call({ value: 4, where: "" });
    check(r.ok && r.value?.value === 8, "a node on this host calls a function a node through the tunnel answers");
    page.close();
    host.close();

    bridge.kill();
    await sleep(300);
    bridge = start();
    await until(() => events.some((e) => e.kind === "restored"), 8000);
    check(events.some((e) => e.kind === "restored"), "after the bridge restarts the session is set up again");
    got.length = 0;
    peer.send("again", aPort, "127.0.0.1");
    await until(() => got.length > 0, 3000);
    check(got.some((x) => x.id === a && x.text === "again"), "the lent port reaches the client again");

    /* the passphrase itself opens nothing: an open must prove a fresh nonce */
    const ws = new WebSocket(`ws://127.0.0.1:${PORT}`);
    const answer = await new Promise((resolve, reject) => {
        ws.onerror = () => reject(new Error("cannot reach the bridge"));
        ws.onopen = () => ws.send(JSON.stringify({ op: "open", seq: 1, proto: 15, passphrase: "tunnel-test" }));
        ws.onmessage = (m) => resolve(JSON.parse(String(m.data)));
    });
    ws.close();
    check(answer.ok === false && /challenge/.test(answer.error), `an open with no proof is refused (${answer.error})`);
} catch (e) {
    console.log(`FAIL ${e?.message ?? e}`);
    failed++;
} finally {
    net?.shutdown();
    peer.close();
    bridge.kill();
}
process.exit(failed ? 1 : 0);
