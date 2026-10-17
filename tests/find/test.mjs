// Finding bridges over mDNS against two freshly built bridges, both advertising on loopback
// only: each lists the other, and both answer the shared name. With Rant's web package, its
// findBridges reads the list too.   node tests/find/test.mjs <rant-bridge exe> [<web package dir>]

import { spawn } from "node:child_process";
import dgram from "node:dgram";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

const [exe, pkg] = process.argv.slice(2);
const base = 18480 + (process.pid % 200);
const ports = [base, base + 1];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failed = 0;
const check = (ok, what) => { console.log(`${ok ? "ok  " : "FAIL"} ${what}`); if (!ok) failed++; };

/* each a home of its own, or a bridge writes its state over that of the user's bridge */
const bridges = ports.map((port) => {
    const dir = mkdtempSync(join(tmpdir(), "find-test-"));
    writeFileSync(join(dir, "bridge.yaml"), `port: ${port}\nbind: 127.0.0.1\npassphrase: find-test\nwebrtc: false\nadvertise: true\n`);
    return spawn(exe, ["--config", join(dir, "bridge.yaml")], { env: { ...process.env, RANT_HOME: dir }, stdio: ["ignore", "ignore", "inherit"] });
});

async function list(port) {
    const ws = new WebSocket(`ws://127.0.0.1:${port}`);
    await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error("no bridge")); });
    const reply = await new Promise((res) => { ws.onmessage = (m) => res(JSON.parse(m.data)); ws.send(JSON.stringify({ op: "bridges", seq: 1 })); });
    ws.close();
    return reply.bridges ?? [];
}

/* a plain resolver's query for the shared name, from a port other than 5353 */
function resolve(name) {
    return new Promise((res) => {
        const s = dgram.createSocket("udp4");
        const labels = name.split(".").flatMap((l) => [l.length, ...Buffer.from(l)]);
        const q = Buffer.from([0x12, 0x34, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, ...labels, 0, 0, 1, 0, 1]);
        const answers = [];
        s.on("message", (m) => { if (m[0] === 0x12 && m[1] === 0x34 && m.length >= 4) answers.push([...m.subarray(-4)].join(".")); });
        s.bind(0, "127.0.0.1", () => {
            s.setMulticastInterface("127.0.0.1");
            s.send(q, 5353, "224.0.0.251");
            setTimeout(() => { s.close(); res(answers); }, 1000);
        });
    });
}

try {
    let found = [];
    for (let tries = 0; tries < 30; tries++) {
        await sleep(300);
        try { found = await list(ports[0]); } catch { continue; }
        if (found.some((b) => b.url === `ws://127.0.0.1:${ports[1]}`)) break;
    }
    check(found.some((b) => b.self && b.url === `ws://127.0.0.1:${ports[0]}`), "a bridge lists itself");
    check(found.some((b) => !b.self && b.url === `ws://127.0.0.1:${ports[1]}`), "a bridge lists the other bridge on its network");
    const other = await list(ports[1]);
    check(other.some((b) => !b.self && b.url === `ws://127.0.0.1:${ports[0]}`), "and the other lists it back");
    const answers = await resolve("rant-bridges.local");
    check(answers.length > 0 && answers.every((a) => a === "127.0.0.1"), `the shared name resolves to a bridge (${answers.join(", ")})`);
    if (pkg) {
        const { findBridges } = await import(pathToFileURL(join(pkg, "dist/tunnel.js")).href);
        const seen = await findBridges(`ws://127.0.0.1:${ports[1]}`);
        check(seen.length === found.length, "Rant's findBridges reads the same list");
    }
} catch (e) {
    console.log(`FAIL ${e?.message ?? e}`);
    failed++;
} finally {
    bridges.forEach((b) => b.kill());
}
process.exit(failed ? 1 : 0);
