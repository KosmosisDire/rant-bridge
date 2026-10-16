/* The net object of Rant built to wasm (rant/bindings/web, contract in rant/spec/web.md) over
 * the bridge: the node runs in this page and borrows UDP sockets on the bridge's host, so to
 * the mesh it is a node of that host. The bridge moves datagrams and reads none. */

import { RantNode, type NodeOptions, type RantEvent } from "./node.js";

export interface TunnelOptions {
    passphrase?: string;
    /* the name of the bridge session in its logs, and the hostname the node sees */
    name?: string;
    transport?: "auto" | "websocket";
    rtc?: typeof RTCPeerConnection;
    iceServers?: RTCIceServer[];
    rtcTimeoutMs?: number;
    probeIntervalMs?: number;
    /* sockets bound ahead, one for each node opened before the next refill lands */
    spares?: number;
    /* the session's events: the WebRTC state, errors */
    onEvent?: (e: RantEvent) => void;
}

/* What crossed the tunnel, for a benchmark or a status line. */
export interface TunnelStats { sentPackets: number; sentBytes: number; receivedPackets: number; receivedBytes: number; dropped: number }

interface Sink { deliver(id: number, naddr: number, port: number, bytes: Uint8Array): void }

/* A naddr is the four address bytes as one little endian u32, as wasm memory holds them. */
const naddr = (dotted: string): number => {
    const p = dotted.split(".").map(Number);
    return (p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)) >>> 0;
};
const dotted = (n: number): string => [n & 255, (n >>> 8) & 255, (n >>> 16) & 255, n >>> 24].join(".");

const UDP = 9;
const HEADER = 14;

export class TunnelNet {
    readonly stats: TunnelStats = { sentPackets: 0, sentBytes: 0, receivedPackets: 0, receivedBytes: 0, dropped: 0 };
    private sink: Sink | null = null;
    private nextId = 1;
    private nextSock = 1;
    private sockets = new Map<number, { sock: number; via: number }>();    /* the node's ids */
    private ids = new Map<number, number>();                                 /* bridge socket to id */
    private spares: { sock: number; port: number }[] = [];
    private ifaces: { addr: number; mask: number }[] = [];

    private constructor(private readonly link: RantNode, private readonly spareCount: number, private readonly host: string) {}

    /* Opens a tunnel session and binds the sockets the first node takes at once. */
    static async connect(url: string, opts: TunnelOptions = {}): Promise<TunnelNet> {
        const { spares = 2, ...rest } = opts;
        const link = await RantNode.connect(url, { ...rest, tunnel: true } as NodeOptions);
        const net = new TunnelNet(link, spares, opts.name ?? "web");
        net.ifaces = (link.tunnelInfo?.interfaces ?? []).map((f) => ({ addr: naddr(f.addr), mask: naddr(f.mask) }));
        link.onUdp = (frame) => net.received(frame);
        await net.prepare();
        return net;
    }

    /* "webrtc" or "websocket", and the round trip the bridge measures. */
    get connection() { return this.link.connection; }

    /* Binds spare sockets until there are enough for the next nodes. */
    async prepare(): Promise<void> {
        while (this.spares.length < this.spareCount) {
            const sock = this.nextSock++;
            const r = await this.link.request({ op: "udp_bind", sock, port: 0 });
            this.spares.push({ sock, port: r.port });
        }
    }

    /* Ends the tunnel session and every socket in it. */
    shutdown(): void { this.link.close(); }

    /* ---- the net object ---- */

    attach(sink: Sink): void { this.sink = sink; }

    open(): number {
        const id = this.nextId++;
        this.sockets.set(id, { sock: 0, via: 0 });
        return id;
    }

    bind(id: number, ifNaddr: number, port: number, reuse: boolean): number {
        const s = this.sockets.get(id);
        if (!s || s.sock) return 0;
        if (port === 0) {
            /* a node needs its port now, so it takes a socket bound ahead to any address */
            const spare = ifNaddr ? undefined : this.spares.shift();
            if (!spare) return 0;
            this.adopt(id, s, spare.sock);
            this.prepare().catch((e) => this.warn("binding a spare socket", e));
            return spare.port;
        }
        this.adopt(id, s, this.nextSock++);
        this.ask({ op: "udp_bind", sock: s.sock, port, if: dotted(ifNaddr), reuse }, `binding port ${port}`);
        return port;
    }

    setMulticastInterface(id: number, ifNaddr: number): void {
        const s = this.sockets.get(id);
        if (s) s.via = ifNaddr;
    }
    setMulticastTTL(id: number, ttl: number): void { this.option(id, { ttl }); }
    setMulticastLoopback(id: number, on: boolean): void { this.option(id, { loop: on }); }
    ownGroupsOnly(id: number): void { this.option(id, { own_groups: true }); }

    membership(id: number, group: number, ifNaddr: number, join: boolean): boolean {
        const s = this.sockets.get(id);
        if (!s?.sock) return false;
        this.ask({ op: "udp_member", sock: s.sock, group: dotted(group), if: dotted(ifNaddr), join }, `joining ${dotted(group)}`);
        return true;
    }

    /* bytes views wasm memory and is valid only during this call, so it is copied here. */
    send(id: number, bytes: Uint8Array, to: number, port: number): number {
        const s = this.sockets.get(id);
        if (!s?.sock) return -1;
        const f = new Uint8Array(HEADER + bytes.length);
        const v = new DataView(f.buffer);
        f[0] = UDP;
        v.setUint16(2, s.sock, true);
        v.setUint32(4, to, true);
        v.setUint16(8, port, true);
        v.setUint32(10, s.via, true);
        f.set(bytes, HEADER);
        if (!this.link.sendUdp(f)) { this.stats.dropped++; return -1; }
        this.stats.sentPackets++;
        this.stats.sentBytes += f.length;
        return bytes.length;
    }

    close(id: number): void {
        const s = this.sockets.get(id);
        this.sockets.delete(id);
        if (s?.sock) {
            this.ids.delete(s.sock);
            this.ask({ op: "udp_close", sock: s.sock }, "closing a socket");
        }
    }

    interfaces(): { addr: number; mask: number }[] { return this.ifaces; }
    hostname(): string { return this.host; }

    /* ---- inside ---- */

    private adopt(id: number, s: { sock: number }, sock: number): void {
        s.sock = sock;
        this.ids.set(sock, id);
    }

    private option(id: number, o: Record<string, unknown>): void {
        const s = this.sockets.get(id);
        if (s?.sock) this.ask({ op: "udp_option", sock: s.sock, ...o }, "setting a socket option");
    }

    private ask(req: Record<string, unknown>, what: string): void {
        this.link.request(req).catch((e) => this.warn(what, e));
    }

    private warn(what: string, e: unknown): void {
        console.warn(`rant tunnel: ${what}: ${(e as Error)?.message ?? e}`);
    }

    private received(frame: Uint8Array): void {
        if (frame.length < HEADER) return;
        const v = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
        const id = this.ids.get(v.getUint16(2, true));
        if (id === undefined) return;
        this.stats.receivedPackets++;
        this.stats.receivedBytes += frame.length;
        this.sink?.deliver(id, v.getUint32(4, true), v.getUint16(8, true), frame.subarray(HEADER));
    }
}
