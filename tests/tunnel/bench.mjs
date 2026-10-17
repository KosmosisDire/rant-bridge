// The page of the tunnel benchmark: Rant built to wasm (rant/bindings/web bench) through
// TunnelNet. window.bench holds the state run.mjs reads:
//   started  performance.now() when the measured phase began
//   result   the JSON line, with what crossed the tunnel or the socket
//   error    why it stopped

import { TunnelNet } from "/rant/tunnel.js";

const q = new URLSearchParams(location.search);
const bridge = q.get("bridge") ?? "ws://127.0.0.1:7490";
const passphrase = q.get("pass") ?? "";
const transport = q.get("transport") === "websocket" ? "websocket" : "auto";
const args = (q.get("args") ?? "ping").split(",");
const bench = (window.bench = { started: 0, result: null, error: null, carrier: null, isolated: crossOriginIsolated });
const log = (t) => { document.getElementById("log").textContent += t + "\n"; };

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

wasm().catch((e) => {
    bench.error = String(e?.message ?? e);
    log("error: " + bench.error);
});
