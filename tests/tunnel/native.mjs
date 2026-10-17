// Runs native.c's program against a freshly built bridge on loopback and restarts the bridge
// when the program asks.   node tests/tunnel/native.mjs <rant-bridge exe> <tunnel_native exe>

import { spawn } from "node:child_process";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";

const [exe, program] = process.argv.slice(2);
const PORT = 17680 + (process.pid % 200);
const domain = 230 + (process.pid % 20);
const cfg = join(mkdtempSync(join(tmpdir(), "tunnel-native-")), "bridge.yaml");
writeFileSync(cfg, `port: ${PORT}\nbind: 127.0.0.1\npassphrase: native-test\nwebrtc: false\nadvertise: false\n`);

/* a home of its own, or the bridge writes its state over that of the user's bridge */
const start = () => spawn(exe, ["--config", cfg], { env: { ...process.env, RANT_HOME: dirname(cfg) }, stdio: ["ignore", "ignore", "inherit"] });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let bridge = start();
await sleep(500);

const child = spawn(program, [`ws://127.0.0.1:${PORT}`, "native-test", String(domain)], { stdio: ["pipe", "pipe", "inherit"] });
const timer = setTimeout(() => { console.log("FAIL the program did not finish in time"); child.kill(); }, 90000);
child.stdout.on("data", async (d) => {
    process.stdout.write(d);
    if (!String(d).includes("restart now")) return;
    bridge.kill();
    await new Promise((r) => bridge.once("exit", r));
    await sleep(1000);
    bridge = start();
    child.stdin.write("go\n");
});
const code = await new Promise((r) => child.on("exit", r));
clearTimeout(timer);
bridge.kill();
process.exit(code ?? 1);
