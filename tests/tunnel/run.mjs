// The tunnel benchmark: Rant built to wasm in a page through TunnelNet, against today's
// entity protocol through RantNode, against Rant in Node.js over real UDP and native Rant.
// Every pinger talks to the same native echo node. Run from the bridge checkout:
//
//   node tests/tunnel/run.mjs [--rant-web PATH] [--only WORKLOAD,...] [--clients NAME,...]
//                             [--throttle N] [--out FILE]
//
// --rant-web is the Rant checkout with bin/rant_web_bench.exe and bin/web (cmake --preset web).
// A page's CPU is its main thread busy time over the measured phase (DevTools TaskDuration),
// the others' is process CPU over the same phase. The bridge's CPU is measured around each run.

import { spawn, spawnSync } from "node:child_process";
import { createServer } from "node:http";
import { readFileSync, writeFileSync, mkdtempSync, existsSync, appendFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, extname, resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const bridgeRoot = resolve(here, "../..");
const argv = process.argv.slice(2);
const arg = (k, d) => { const i = argv.indexOf(k); return i < 0 ? d : argv[i + 1]; };
const rantWeb = resolve(arg("--rant-web", resolve(bridgeRoot, "../rant-web")));
const only = arg("--only", "")?.split(",").filter(Boolean) ?? [];
const throttle = Number(arg("--throttle", "1"));
const pick = arg("--clients", "")?.split(",").filter(Boolean) ?? [];
const out = arg("--out", join(tmpdir(), "tunnel-bench.jsonl"));
const DOMAIN = "160";
const HTTP = 8095, BRIDGE = 7490, CDP = 9333;
/* EXTERNAL: a bridge and a reliable echo already run at BRIDGE_HOST, such as in WSL */
const HOST = process.env.BRIDGE_HOST ?? "127.0.0.1";
const EXTERNAL = !!process.env.EXTERNAL;
const PAGES = new Set(["tunnel-webrtc", "tunnel-ws", "client-webrtc", "client-ws"]);
const exe = (p) => (process.platform === "win32" ? p + ".exe" : p);
const benchExe = join(rantWeb, "bin", exe("rant_web_bench"));
const bridgeExe = process.env.BRIDGE_EXE ?? join(bridgeRoot, "bin", exe("rant-bridge"));
const chrome = process.env.CHROME ?? "C:/Program Files/Google/Chrome/Application/chrome.exe";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const procs = [];

function start(cmd, args, name) {
    const p = spawn(cmd, args, { stdio: ["ignore", "pipe", "pipe"] });
    p.name = name;
    p.lines = [];
    let buf = "";
    p.stdout.on("data", (d) => { buf += d; const parts = buf.split(/\r?\n/); buf = parts.pop(); p.lines.push(...parts); });
    p.stderr.on("data", (d) => { if (process.env.VERBOSE) process.stderr.write(`[${name}] ${d}`); });
    procs.push(p);
    return p;
}
function stop(p) { try { p.kill(); } catch { /* gone */ } }
process.on("exit", () => procs.forEach(stop));

function cpuMs(pid) {
    if (process.platform !== "win32") {
        const f = readFileSync(`/proc/${pid}/stat`, "utf8").split(" ");
        return (Number(f[13]) + Number(f[14])) * 10;
    }
    const r = spawnSync("powershell", ["-NoProfile", "-Command", `(Get-Process -Id ${pid}).TotalProcessorTime.TotalMilliseconds`], { encoding: "utf8" });
    return Number(r.stdout.trim());
}

/* ---- the pieces ---- */

const types = { ".html": "text/html", ".mjs": "text/javascript", ".js": "text/javascript", ".wasm": "application/wasm" };
function serve() {
    const roots = [["/wasm/", join(rantWeb, "bin", "web")], ["/client/", join(bridgeRoot, "client", "dist")], ["/", here]];
    const server = createServer((req, res) => {
        const path = decodeURIComponent(req.url.split("?")[0]);
        for (const [prefix, dir] of roots) {
            if (!path.startsWith(prefix)) continue;
            const file = join(dir, path.slice(prefix.length));
            if (!existsSync(file)) break;
            /* cross origin isolation gives performance.now 5 us steps instead of 100 */
            res.writeHead(200, { "content-type": types[extname(file)] ?? "application/octet-stream", "cache-control": "no-store",
                                 "cross-origin-opener-policy": "same-origin", "cross-origin-embedder-policy": "require-corp" });
            res.end(readFileSync(file));
            return;
        }
        res.writeHead(404).end();
    });
    server.listen(HTTP, "127.0.0.1");
    return server;
}

function startBridge() {
    const dir = mkdtempSync(join(tmpdir(), "tunnel-bench-"));
    const config = join(dir, "bridge.yaml");
    /* sessions outlive the run: reaping a WebRTC session after its grace crashes the bridge
       (a bug of the released bridges too), which would end the matrix */
    writeFileSync(config, `port: ${BRIDGE}\nbind: 127.0.0.1\npassphrase: bench\nwebrtc: true\ngrace_ms: ${process.env.GRACE_MS ?? 3600000}\n` +
                          (process.env.BRIDGE_MTU ? `mtu: ${process.env.BRIDGE_MTU}\n` : ""));
    const p = start(bridgeExe, ["--config", config], "bridge");
    p.on("exit", (code) => console.error(`the bridge exited with ${code}`));
    return p;
}

class Cdp {
    static async open(url) {
        const t = await (await fetch(`http://127.0.0.1:${CDP}/json/new?${encodeURIComponent(url)}`, { method: "PUT" })).json();
        const c = new Cdp();
        c.id = t.id;
        c.ws = new WebSocket(t.webSocketDebuggerUrl);
        c.next = 0;
        c.waiting = new Map();
        c.ws.onmessage = (m) => {
            const r = JSON.parse(m.data);
            if (r.method && process.env.VERBOSE) {
                if (r.method === "Runtime.consoleAPICalled") console.error("[page]", r.params.args.map((x) => x.value ?? x.description).join(" "));
                if (r.method === "Runtime.exceptionThrown") console.error("[page exception]", r.params.exceptionDetails?.exception?.description ?? r.params.exceptionDetails?.text);
            }
            c.waiting.get(r.id)?.(r);
            c.waiting.delete(r.id);
        };
        await new Promise((r) => { c.ws.onopen = r; });
        return c;
    }
    send(method, params = {}) {
        const id = ++this.next;
        return new Promise((r) => { this.waiting.set(id, r); this.ws.send(JSON.stringify({ id, method, params })); });
    }
    async eval(expr) { return (await this.send("Runtime.evaluate", { expression: expr, returnByValue: true })).result?.result?.value; }
    async metrics() {
        const r = await this.send("Performance.getMetrics");
        return Object.fromEntries((r.result?.metrics ?? []).map((m) => [m.name, m.value]));
    }
    async close() { this.ws.close(); await fetch(`http://127.0.0.1:${CDP}/json/close/${this.id}`); }
}

async function startChrome() {
    const dir = mkdtempSync(join(tmpdir(), "tunnel-chrome-"));
    const p = start(chrome, ["--headless=new", `--remote-debugging-port=${CDP}`, `--user-data-dir=${dir}`, "--no-first-run",
                             "--no-default-browser-check", "--disable-background-timer-throttling",
                             "--disable-renderer-backgrounding", "about:blank"], "chrome");
    for (let i = 0; i < 100; i++) {
        try { await fetch(`http://127.0.0.1:${CDP}/json/version`); return p; } catch { await sleep(100); }
    }
    throw new Error("chrome did not open its debugging port");
}

/* The functions a page spent its samples in, by self time, with what called the top ones. */
function printProfile(p) {
    const byId = new Map(p.nodes.map((n) => [n.id, n]));
    const parent = new Map();
    for (const n of p.nodes) for (const c of n.children ?? []) parent.set(c, n.id);
    const self = new Map();
    const counts = new Map();
    for (const id of p.samples) counts.set(id, (counts.get(id) ?? 0) + 1);
    let total = 0;
    for (const [id, k] of counts) {
        const f = byId.get(id).callFrame;
        const name = `${f.functionName || "(anonymous)"} ${f.url ? f.url.split("/").pop() + ":" + f.lineNumber : ""}`.trim();
        self.set(name, (self.get(name) ?? 0) + k);
        total += k;
    }
    console.log(`profile: ${total} samples`);
    for (const [name, k] of [...self].sort((a, b) => b[1] - a[1]).slice(0, 30))
        console.log(`${String(k).padStart(7)} ${(100 * k / total).toFixed(1).padStart(5)}%  ${name}`);
}

/* ---- one run ---- */

async function page(mode, transport, args, rate) {
    const url = `http://127.0.0.1:${HTTP}/bench.html?mode=${mode}&transport=${transport}&bridge=ws://${HOST}:${BRIDGE}` +
                `&pass=bench&args=${args.join(",")}`;
    const c = await Cdp.open("about:blank");
    await c.send("Performance.enable");
    if (process.env.VERBOSE) await c.send("Runtime.enable");
    if (rate > 1) await c.send("Emulation.setCPUThrottlingRate", { rate });
    await c.send("Page.navigate", { url });
    let m0 = null, b = null;
    const t0 = Date.now();
    const profile = !!process.env.PROFILE;
    if (profile) { await c.send("Profiler.enable"); await c.send("Profiler.setSamplingInterval", { interval: 100 }); }
    while (Date.now() - t0 < 120000) {
        await sleep(50);      /* the start marker is read promptly, so a short run's CPU is right */
        b = await c.eval("JSON.stringify(window.bench ?? null)").then((s) => (s ? JSON.parse(s) : null));
        if (!b) continue;
        if (b.started && !m0) { m0 = await c.metrics(); if (profile) await c.send("Profiler.start"); }
        if (b.result || b.error) break;
    }
    const m1 = await c.metrics();
    if (profile && m0) printProfile((await c.send("Profiler.stop")).result.profile);
    await c.close();
    if (!b?.result) return { error: b?.error ?? "no result in 120 s" };
    return { ...b.result, isolated: b.isolated, cpu_ms: m0 ? Math.round((m1.TaskDuration - m0.TaskDuration) * 1000) : null,
             script_ms: m0 ? Math.round((m1.ScriptDuration - m0.ScriptDuration) * 1000) : null,
             heap_mb: +(m1.JSHeapUsedSize / 1048576).toFixed(1) };
}

async function proc(cmd, args) {
    const p = start(cmd, args, "pinger");
    const t0 = Date.now();
    while (Date.now() - t0 < 120000) {
        const line = p.lines.find((l) => l.startsWith("{\"mode\""));
        if (line) { stop(p); return JSON.parse(line); }
        if (p.exitCode !== null) break;
        await sleep(100);
    }
    stop(p);
    return { error: "no result" };
}

const clients = [
    ["native", (a) => proc(benchExe, a)],
    ["wasm-node", (a) => proc(process.execPath, [join(rantWeb, "bindings", "web", "bench", "node.mjs"), ...a])],
    ["tunnel-node", (a) => proc(process.execPath, [join(here, "node-tunnel.mjs"), `ws://${HOST}:${BRIDGE}`, ...a])],
    ["tunnel-webrtc", (a) => page("wasm", "auto", a, throttle)],
    ["tunnel-ws", (a) => page("wasm", "websocket", a, throttle)],
    ["client-webrtc", (a) => page("client", "auto", a, throttle)],
    ["client-ws", (a) => page("client", "websocket", a, throttle)],
];

const workloads = [
    ["rtt-64B", ["ping", "--reliable", "--size", "64", "--count", "1000", "--rate", "100"]],
    ["rtt-16KB", ["ping", "--reliable", "--size", "16384", "--count", "500", "--rate", "50"]],
    ["tput-1KB", ["ping", "--reliable", "--size", "1024", "--count", "40000", "--window", "32"]],
    ["tput-64KB", ["ping", "--reliable", "--size", "65536", "--count", "2000", "--window", "8"]],
    ["idle", ["idle", "--seconds", "20"]],
];

/* ---- main ---- */

const server = serve();
const bridge = EXTERNAL ? null : startBridge();
const wanted = clients.map(([n]) => n).filter((n) => !pick.length || pick.includes(n));
if (wanted.some((n) => PAGES.has(n))) {
    await startChrome();
    /* the first peer connection of a browser stalls its page while WebRTC starts, so that
       cost lands here and not in a run */
    const w = await Cdp.open("about:blank");
    await w.send("Runtime.evaluate", { expression: "(async () => { const p = new RTCPeerConnection(); p.createDataChannel('w'); await p.setLocalDescription(); await new Promise((r) => setTimeout(r, 1500)); p.close(); })()", awaitPromise: true });
    await w.close();
}
await sleep(1000);
const rows = [];
for (const [wname, wargs] of workloads) {
    if (only.length && !only.some((o) => wname.startsWith(o) || o === wname)) continue;
    const reliable = wargs.includes("--reliable");
    const echo = EXTERNAL ? null : start(benchExe, ["echo", "--domain", DOMAIN, ...(reliable ? ["--reliable"] : [])], "echo");
    /* idle nodes run on a mesh of ten others, so discovery has something to do */
    const crowd = wname === "idle" ? Array.from({ length: 10 }, (_, i) =>
        start(benchExe, ["idle", "--domain", DOMAIN, "--seconds", "600"], `crowd${i}`)) : [];
    await sleep(1500);
    for (const [cname, run] of clients) {
        if (pick.length && !pick.includes(cname)) continue;
        const cpu = (p) => (p ? cpuMs(p.pid) : 0);
        const b0 = cpu(bridge), e0 = cpu(echo);
        const r = await run([...wargs, "--domain", DOMAIN, ...(process.env.EXTRA ? process.env.EXTRA.split(" ") : [])]);
        const row = { workload: wname, client: cname, throttle, ...r, bridge_cpu_ms: bridge ? Math.round(cpu(bridge) - b0) : null,
                      echo_cpu_ms: echo ? Math.round(cpu(echo) - e0) : null };
        rows.push(row);
        appendFileSync(out, JSON.stringify(row) + "\n");
        console.log(JSON.stringify(row));
        await sleep(500);
    }
    if (echo) stop(echo);
    crowd.forEach(stop);
    await sleep(1000);
}
server.close();
console.log(`\nwrote ${out}`);
process.exit(0);
