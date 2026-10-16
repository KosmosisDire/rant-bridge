// The page of the tunnel benchmark. mode=wasm runs Rant built to wasm (rant/bindings/web
// bench) through TunnelNet, mode=client runs the same ping through the bridge's entity
// protocol with RantNode. window.bench holds the state run.mjs reads:
//   started  performance.now() when the measured phase began
//   result   the JSON line, with what crossed the tunnel or the socket
//   error    why it stopped

import { RantNode, TunnelNet } from "/client/rant.js";

const q = new URLSearchParams(location.search);
const bridge = q.get("bridge") ?? "ws://127.0.0.1:7490";
const passphrase = q.get("pass") ?? "";
const transport = q.get("transport") === "websocket" ? "websocket" : "auto";
const args = (q.get("args") ?? "ping").split(",");
const bench = (window.bench = { started: 0, result: null, error: null, carrier: null });
const log = (t) => { document.getElementById("log").textContent += t + "\n"; };

function option(name, fallback) {
    const i = args.indexOf(name);
    return i < 0 ? fallback : args[i + 1];
}

async function wasm() {
    const { default: createBench } = await import("/wasm/rant_web_bench.mjs");
    const net = await TunnelNet.connect(bridge, { passphrase, name: "bench-tunnel", transport,
                                                  onEvent: (e) => console.log(Math.round(performance.now()), "event", e.kind, e.text) });
    console.log(Math.round(performance.now()), "tunnel up over", net.connection.carrier);
    bench.carrier = net.connection.carrier;
    await createBench({
        arguments: args,
        rantNet: net,
        print: (line) => {
            log(line);
            if (line.startsWith("{\"phase\"")) bench.started = performance.now();
            else if (line.startsWith("{\"mode\"")) {
                bench.result = { ...JSON.parse(line), carrier: net.connection.carrier, tunnel: { ...net.stats } };
            }
        },
        printErr: (line) => log("! " + line),
    });
}

/* The ping of bench.cpp over RantNode: the same payload, timing and counts. */
async function client() {
    const mode = args[0];
    const domain = Number(option("--domain", "160"));
    const reliable = args.includes("--reliable");
    const size = Math.max(16, Number(option("--size", "64")));
    const count = Number(option("--count", "1000"));
    const rate = Number(option("--rate", "100"));
    const window_ = Number(option("--window", "0"));
    const node = await RantNode.connect(bridge, { passphrase, name: "bench-client", domain, transport, matchWaitMs: -1,
                                                  recvBufferBytes: 8 << 20, sendBufferBytes: 8 << 20, disableShm: true });
    bench.carrier = node.connection.carrier;
    const opts = { reliable, queueBytes: 8 << 20, ...(size > 1350 ? { maxMessageBytes: size } : {}) };

    if (mode === "idle") {
        const seconds = Number(option("--seconds", "10"));
        await node.subscriber("bench/idle", null, () => {}, opts);
        bench.started = performance.now();
        await new Promise((r) => setTimeout(r, seconds * 1000));
        bench.result = { mode: "idle", seconds, peers: (await node.peers()).length, carrier: node.connection.carrier };
        return;
    }

    const rtt = [];
    const seen = new Uint8Array(count);
    let sent = 0, got = 0, late = 0, start = 0, lastSend = 0;
    const payload = new Uint8Array(size).fill(0x5a);
    const view = new DataView(payload.buffer);
    const now = () => performance.now() * 1000;

    const pub = await node.publisher("bench/ping", null, opts);
    let wake = null;
    await node.subscriber("bench/pong", null, (value) => {
        const d = value instanceof Uint8Array ? value : new Uint8Array(value);
        if (d.length < 16) return;
        const v = new DataView(d.buffer, d.byteOffset, d.byteLength);
        const at = Number(v.getBigUint64(8, true));
        if (at < start) { late++; return; }
        rtt.push(now() - at);
        const seq = Number(v.getBigUint64(0, true));
        if (seq < count) seen[seq] = 1;
        got++;
        wake?.();
    }, opts);

    const t0 = performance.now();
    while (!(pub.matchCount > 0)) {
        if (performance.now() - t0 > 20000) throw new Error("no echo node matched in 20 s");
        await new Promise((r) => setTimeout(r, 50));
    }
    await new Promise((r) => setTimeout(r, 500));
    start = now();
    bench.started = performance.now();

    const sendOne = () => {
        view.setBigUint64(0, BigInt(sent), true);
        view.setBigUint64(8, BigInt(Math.round(now())), true);
        pub.send(payload);
        sent++;
        lastSend = now();
    };
    if (window_ > 0) {
        while (sent < count) {
            while (sent < count && sent - got < window_) sendOne();
            await new Promise((r) => { wake = r; setTimeout(r, 50); });
        }
    } else {
        const gap = 1000 / Math.max(rate, 1);
        let next = performance.now();
        while (sent < count) {
            while (sent < count && performance.now() >= next) { sendOne(); next += gap; }
            await new Promise((r) => setTimeout(r, Math.max(0, next - performance.now())));
        }
    }
    while (got < count && now() - lastSend < 2e6) await new Promise((r) => { wake = r; setTimeout(r, 50); });

    rtt.sort((a, b) => a - b);
    const pct = (p) => (rtt.length ? Math.round(rtt[Math.min(rtt.length - 1, Math.floor(p * rtt.length))]) : 0);
    const secs = (now() - start) / 1e6;
    const lost = [];
    for (let i = 0; i < sent && lost.length < 12; i++) if (!seen[i]) lost.push(i);
    bench.result = {
        mode: "ping", reliable, size, count, rate, window: window_, sent, got, late,
        p50_us: pct(0.5), p90_us: pct(0.9), p99_us: pct(0.99), max_us: Math.round(rtt.at(-1) ?? 0),
        mean_us: Math.round(rtt.reduce((a, b) => a + b, 0) / Math.max(rtt.length, 1)),
        seconds: secs, msgs_per_s: Math.round(got / secs), lost, carrier: node.connection.carrier,
    };
    log(JSON.stringify(bench.result));
}

(q.get("mode") === "client" ? client() : wasm()).catch((e) => {
    bench.error = String(e?.message ?? e);
    log("error: " + bench.error);
});
