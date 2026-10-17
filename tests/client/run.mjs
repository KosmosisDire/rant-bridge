/* End to end: a fresh bridge with a scratch config, two client nodes on domain 150, every
 * pattern, reflection, auth, and a resumed and a lost session. node run.mjs <rant-bridge> */

import { spawn } from "node:child_process";
import { mkdtempSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import * as rant from "../../client/dist/rant.js";

const exe = process.argv[2];
const port = 7600 + Math.floor(Math.random() * 300);
const home = mkdtempSync(join(tmpdir(), "rant-bridge-test-"));
writeFileSync(join(home, "bridge.yaml"),
    `port: ${port}\nbind: 127.0.0.1\npassphrase: test-pass\ngrace_ms: 1500\ndetach_ms: 1000\nice: []\n`);

const bridge = spawn(exe, ["--config", join(home, "bridge.yaml")], { env: { ...process.env, RANT_HOME: home }, stdio: ["ignore", "pipe", "pipe"] });
let log = "";
bridge.stdout.on("data", (d) => { log += d; });
bridge.stderr.on("data", (d) => { log += d; });
const url = `ws://127.0.0.1:${port}`;
const fail = (why) => { console.error("FAIL:", why); console.error(log); bridge.kill(); rmSync(home, { recursive: true, force: true }); process.exit(1); };
const watchdog = setTimeout(() => fail("timed out"), 60000);
process.on("unhandledRejection", (e) => fail(String(e?.stack ?? e)));
process.on("uncaughtException", (e) => fail(String(e?.stack ?? e)));
const until = async (what, cond, ms = 5000) => {
    const end = Date.now() + ms;
    while (!(await cond())) { if (Date.now() > end) fail(what); await new Promise((r) => setTimeout(r, 20)); }
};
const check = (what, ok) => { if (!ok) fail(what); console.log("ok", what); };

await until("the bridge listens", () => log.includes("listening"));

const opts = { passphrase: "test-pass", domain: 150, announceIntervalMs: 200 };
const robot = await rant.RantNode.connect(url, { ...opts, name: "robot" });
const dash = await rant.RantNode.connect(url, { ...opts, name: "dashboard" });
check("both nodes open over the websocket", robot.connection.state === "open" && dash.connection.carrier === "websocket");

/* a wrong passphrase is refused */
let refused = false;
try { await rant.RantNode.connect(url, { ...opts, passphrase: "nope" }); } catch (e) { refused = /passphrase/.test(String(e?.message)); }
check("a wrong passphrase is refused", refused);

/* typed pub and sub: your member names, 64 bit values, struct arrays, NaN */
const Reading = rant.struct("Reading", {
    stampUs: rant.u64, value: rant.f64, label: rant.string(16), mode: rant.enumOf({ Idle: 0, Run: 1 }),
    points: rant.array(rant.types.Float3), samples: rant.array(rant.f32),
});
const pub = await robot.publisher("test/reading", Reading, { reliable: true });
const got = [];
const sub = await dash.subscriber("test/reading", Reading, (v) => got.push(v), { reliable: true });
await until("the topic matches", () => pub.matchCount > 0);
pub.send({ stampUs: 18000000000000000123n, value: NaN, label: "front", mode: "Run",
           points: [{ x: 1, y: 2, z: 3 }, { x: 4, y: 5, z: 6 }], samples: new Float32Array([1.5, 2.5]) });
await until("the reading arrives", () => got.length > 0);
const r0 = got[0];
check("values keep their types", r0.stampUs === 18000000000000000123n && Number.isNaN(r0.value) && r0.label === "front" &&
      r0.mode === "Run" && r0.points[1].z === 6 && r0.samples instanceof Float32Array && r0.samples[1] === 2.5);

/* a large message goes out in chunks and comes back whole */
const Blob1 = rant.struct("Blob1", { data: rant.array(rant.u8) });
const bpub = await robot.publisher("test/blob", Blob1, { reliable: true });
let blob = null;
await dash.subscriber("test/blob", Blob1, (v) => { blob = v; }, { reliable: true });
await until("the blob topic matches", () => bpub.matchCount > 0);
const big = new Uint8Array(200000).map((_, i) => i & 255);
bpub.send({ data: big });
await until("the blob arrives", () => blob !== null, 10000);
check("a chunked message arrives whole", blob.data.length === big.length && blob.data[199999] === big[199999]);

/* functions */
const AddReq = rant.struct("AddReq", { a: rant.i32, b: rant.i32 });
const AddRsp = rant.struct("AddRsp", { sum: rant.i32 });
await robot.functionDefinition("test/add", (q) => ({ sum: q.a + q.b }), AddReq, AddRsp);
const add = await dash.remoteFunction("test/add", AddReq, AddRsp);
await until("the function matches", () => add.matchCount > 0);
const r = await add.call({ a: 2, b: 3 });
check("a function call answers", r.ok && r.value.sum === 5);

/* tasks with progress and cancel */
const Goal = rant.struct("Goal", { steps: rant.u32 });
const Prog = rant.struct("Prog", { done: rant.u32 });
await robot.taskDefinition("test/count", async (g, ctx) => {
    for (let i = 1; i <= g.steps; i++) {
        if (ctx.signal.aborted) throw new rant.CancelledError("stopped", { done: i });
        ctx.progress({ done: i });
        await new Promise((res) => setTimeout(res, 30));
    }
    return { done: g.steps };
}, Goal, Prog, Prog);
const count = await dash.remoteTask("test/count", Goal, Prog, Prog);
await until("the task matches", () => count.matchCount > 0);
const seen = [];
const run = count.call({ steps: 4 }).onProgress((p) => { if (p) seen.push(p.done); });
const done = await run.result;
check("a task streams progress and finishes", done.ok && done.value.done === 4 && seen.join() === "1,2,3,4");
const long = count.call({ steps: 100 });
await new Promise((res) => setTimeout(res, 100));
await long.cancel();
const stopped = await long.result;
check("a task cancels", stopped.status === "cancelled");

/* variables */
const Config = rant.struct("Config", { rateHz: rant.u32 });
const owner = await robot.variableDefinition("test/config", Config, { initial: { rateHz: 10 } });
const remote = await dash.remoteVariable("test/config", Config);
check("a remote variable sees the value", await remote.wait(5000) && remote.get().rateHz === 10);
remote.set({ rateHz: 50 });
await until("a remote set reaches the owner", () => owner.get()?.rateHz === 50);

/* reflection */
const found = await dash.reflection.find("function", "test/add");
check("reflection finds the function", found && found.provides && found.schema?.name === "AddReq");
const peers = await dash.reflection.peers();
check("peers list the robot", peers.some((p) => p.name === "robot"));
await until("the client link is measured", () => dash.connection.rtt.samples > 0, 3000);

/* a short drop resumes the same session: no handle is made again */
const events = [];
dash.onEvent((e) => { events.push(e.kind); if (e.kind === "error") log += `
client: ${e.text}`; });
dash.ws.close();
await until("the session resumes", () => events.includes("resumed"), 5000);
got.length = 0;
pub.send({ stampUs: 1n, value: 1, label: "after", mode: "Idle", points: [], samples: new Float32Array(0) });
await until("delivery continues after the resume", () => got.some((v) => v.label === "after"), 5000);

/* a drop longer than the grace loses the session: every handle is made again */
dash.backoff = 2500;
dash.ws.close();
await until("the session is lost and rebuilt", () => events.includes("session_lost"), 8000);
await until("the topic matches again", () => sub.matchCount >= 0 && pub.matchCount > 0, 5000);
got.length = 0;
pub.send({ stampUs: 2n, value: 2, label: "rebuilt", mode: "Idle", points: [], samples: new Float32Array(0) });
await until("delivery continues after the rebuild", () => got.some((v) => v.label === "rebuilt"), 5000);

robot.close();
dash.close();
clearTimeout(watchdog);
bridge.kill();
await new Promise((res) => setTimeout(res, 200));
rmSync(home, { recursive: true, force: true });
console.log("all passed");
process.exit(0);
