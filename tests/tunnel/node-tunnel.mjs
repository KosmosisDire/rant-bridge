// Runs the wasm bench of rant/bindings/web in Node.js through the bridge tunnel, the same path
// as a page but with no browser, so the two can be told apart:
//   node tests/tunnel/node-tunnel.mjs ws://127.0.0.1:7490 ping --size 64 --count 1000
// RANT_WEB is the Rant checkout holding bin/web and bindings/web/dist. A Node without a global
// WebSocket (before 22) takes the ws package from WS_MODULE.

import { pathToFileURL } from "node:url";
import { join, resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
if (!globalThis.WebSocket) globalThis.WebSocket = (await import(pathToFileURL(process.env.WS_MODULE ?? "ws").href)).WebSocket;
const rantWeb = resolve(process.env.RANT_WEB ?? join(here, "../../../rant"));
const { TunnelNet } = await import(pathToFileURL(join(rantWeb, "bindings/web/dist/tunnel.js")).href);
const { default: createBench } = await import(pathToFileURL(join(rantWeb, "bin/web/rant_web_bench.mjs")).href);

const [url, ...args] = process.argv.slice(2);
const net = await TunnelNet.connect(url, { passphrase: process.env.PASS ?? "bench", name: "bench-node-tunnel", transport: "websocket" });
let done;
const finished = new Promise((r) => { done = r; });
await createBench({
    arguments: args,
    rantNet: net,
    print: (line) => {
        if (line.startsWith("{\"mode\"")) { console.log(JSON.stringify({ ...JSON.parse(line), tunnel: { ...net.stats } })); done(); }
        else console.log(line);
    },
    printErr: (line) => console.error(line),
});
await finished;
net.shutdown();
process.exit(0);
