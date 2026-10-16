/* A RantNode is one full node on the mesh, spoken through the Rant bridge. It reconnects on
 * its own, resumes its session when the bridge still holds it, and never replays old sends. */

import { decode, encode } from "./cbor.js";
import { Frame, Op, Reassembly, Sender, buildFrame, parseFrame } from "./frame.js";
import { Infer, Rename, Schema, Shape, dsl, rename, renamer } from "./schema.js";

const PROTOCOL = 12;

/* ---- public types ------------------------------------------------------------------- */

export type ConnectionState = "connecting" | "open" | "reconnecting" | "closed";
export interface Rtt { us: number; jitterUs: number; samples: number; atUs: number }
export interface Connection { state: ConnectionState; carrier: "webrtc" | "websocket"; rtt: Rtt; missed: number }

/* Every bridge and mesh event, flat. kind is peer_up, peer_down, peer_interest, msg_lost, error,
 * send_error, rtc, warning, resumed, session_lost or discarded. */
export interface RantEvent {
    kind: string; text: string;
    peer?: number; peerName?: string; topic?: string; id?: number; count?: number; error?: number | string;
    [field: string]: unknown;
}

export interface NodeOptions {
    passphrase?: string; name?: string; domain?: number; prefix?: string; maxTopics?: number;
    fetchDetails?: boolean; matchWaitMs?: number; disableLogs?: boolean; disableMeta?: boolean;
    disableErrorLogs?: boolean; disableShm?: boolean; multicastInterface?: string; seedPeers?: string[];
    unicastOnly?: boolean; selfIp?: string; advertisePort?: number; fragmentSize?: number;
    recvBufferBytes?: number; sendBufferBytes?: number; announceIntervalMs?: number;
    peerTimeoutMs?: number; maxPeers?: number;
    /* how often the bridge checks the link to this client, which also bounds how fast a drop shows */
    probeIntervalMs?: number;
    transport?: "auto" | "websocket";
    /* the RTCPeerConnection to use: the global one in browsers, node-datachannel's polyfill in Node */
    rtc?: typeof RTCPeerConnection;
    iceServers?: RTCIceServer[];
    rtcTimeoutMs?: number;
    onEvent?: (e: RantEvent) => void;
}

export interface TopicOptions {
    reliable?: boolean; keepLast?: number; catchUp?: number; maxMessageBytes?: number;
    heartbeatMs?: number; repairDelayMs?: number; backpressureWaitMs?: number; shmMaxBytes?: number;
    queueBytes?: number; maxRateHz?: number; noTimestamp?: boolean; reflect?: boolean;
    /* typed, yet values stay Rant's wire bytes both ways: for a client that has Rant itself */
    raw?: boolean;
}
export interface FunctionOptions { timeoutMs?: number; backpressureWaitMs?: number; keepLast?: number; reflect?: boolean; raw?: boolean }
export interface TaskOptions extends FunctionOptions {
    progressBestEffort?: boolean; progressKeepLast?: number; noCancel?: boolean; exclusive?: boolean; multi?: boolean;
}
export interface VariableOptions<T> {
    initial?: T; readOnly?: boolean; allowForce?: boolean; catchUp?: number; keepLast?: number;
    backpressureWaitMs?: number; onWrite?: boolean; reflect?: boolean; raw?: boolean;
}

export type CallStatus = "ok" | "app_error" | "no_handler" | "timeout" | "peer_lost" | "cancelled" | "no_provider";
const STATUS: Record<number, CallStatus> = { 0: "ok", 1: "app_error", 2: "no_handler", 3: "timeout", 4: "peer_lost", 5: "cancelled", 7: "no_provider" };
const STATUS_TEXT: Record<CallStatus, string> = {
    ok: "", app_error: "app error", no_handler: "no handler", timeout: "timeout", peer_lost: "peer lost",
    cancelled: "cancelled", no_provider: "no provider",
};

export interface Response<T> {
    ok: boolean; status: CallStatus;
    value: T | undefined;      /* whatever came back, on success or failure */
    hasValue: boolean;
    data: Uint8Array;
    message: string;           /* the outcome text, the status's own when the provider sent none */
    provider: number; writtenUs: bigint;
}

export interface Message<T> {
    value: T; data: Uint8Array; publisher: number; writtenUs: bigint; captureUs: bigint; recvUs: number;
}

export interface Request {
    caller: number; callerName: string; writtenUs: bigint;
}

export interface TaskContext<Prg> extends Request {
    progress(value: Prg): void;
    signal: AbortSignal;
}

export type CancelStatus = "ok" | "no_cancel" | "not_pending" | "error";

export interface HandleStats { queuedBytes: number; queuedMsgs: number; oldestAgeUs: number; dropped: number; discarded: number }

export interface Peer {
    id: number; uuid: string; name: string; address: string; active: boolean; lastHeardUs: number;
    epoch: number; catchingUp: boolean; fragmentSize: number;
    rtt: Rtt; meshRtt: Rtt; rttMinUs: number; missed: number;
}
export type EntityKind = "topic" | "function" | "task" | "variable";
export interface SchemaBlock { name: string; hash: string; dsl: string; fields: SchemaRow[] }
export interface SchemaRow {
    path: string; name: string; kind: string; named?: string; elem?: string; elemNamed?: string;
    count?: number; cap?: number; variants?: { name: string; value: number }[];
}
export interface MeshEntity {
    kind: EntityKind; name: string; provides: boolean; consumes: boolean; reliable: boolean;
    providers: number; consumers: number; provider?: number; from?: string; generation: string;
    writable?: boolean; forceable?: boolean; cancellable?: boolean; exclusive?: boolean; multi?: boolean;
    incomplete?: boolean; conflict?: boolean;
    schema?: SchemaBlock; rsp?: SchemaBlock; prg?: SchemaBlock;
}
export interface LogLine { level: "error" | "warn" | "info"; node: string; text: string; wallUs: number; monoUs: number; recvUs: number; writtenUs: number }

export class RantError extends Error {
    constructor(message: string, readonly event?: RantEvent) { super(message); this.name = "RantError"; }
}
/* Thrown by a task handler to answer cancelled, with an optional partial result. */
export class CancelledError<T = unknown> extends Error {
    constructor(message = "cancelled", readonly partial?: T) { super(message); this.name = "CancelledError"; }
}

/* ---- internals shared with rant/video ----------------------------------------------- */

/* One schema as a handle uses it: CBOR with your member names, and where media sits. */
export class Codec {
    readonly typed: boolean;
    readonly block: SchemaBlock | null;
    private names: Rename;
    readonly media: { path: string[]; wire: string; type: string }[] = [];

    constructor(block: any, shape: Shape | undefined, raw = false) {
        this.block = block ? toBlock(block) : null;
        this.typed = !!block && !raw;
        this.names = this.typed ? renamer(shape, this.block!.fields) : null;
        if (!this.typed || !this.block) return;
        if (this.block.name === "VideoFrame" || this.block.name === "Image") this.media.push({ path: [], wire: "", type: this.block.name });
        for (const r of this.block.fields)
            if (r.named === "VideoFrame" || r.named === "Image") this.media.push({ path: this.mine(r.path), wire: r.path, type: r.named });
    }
    /* a wire path in your member names */
    private mine(wire: string): string[] {
        const out: string[] = [];
        let r = this.names;
        for (const seg of wire.split(".")) {
            const pair = r?.members.find(([, w]) => w === seg);
            out.push(pair ? pair[0] : seg);
            r = pair ? r!.sub.get(pair[0]) ?? null : null;
        }
        return out;
    }
    encode(v: unknown): Uint8Array {
        if (this.typed) return encode(rename(v, this.names, true));
        if (v instanceof Uint8Array) return v;
        if (v === undefined || v === null) return new Uint8Array(0);
        throw new RantError("an untyped handle sends a Uint8Array");
    }
    decode(b: Uint8Array): unknown {
        if (!this.typed) return b;
        return rename(decode(b), this.names, false);
    }
}

function toBlock(b: any): SchemaBlock {
    return {
        name: b.name, hash: b.hash, dsl: b.dsl,
        fields: (b.fields ?? []).map((r: any) => ({
            path: r.path, name: r.name, kind: r.kind, named: r.named, elem: r.elem, elemNamed: r.elem_named,
            count: r.count, cap: r.cap, variants: r.variants,
        })),
    };
}

/* rant/video sets this: it gives media values their decode() */
export const media: { attach: ((value: unknown, handle: Handle, publisher: number) => void) | null } = { attach: null };

const snake = (k: string) => k.replace(/[A-Z]/g, (c) => "_" + c.toLowerCase());
function wireOptions(o: object): Record<string, unknown> {
    const out: Record<string, unknown> = {};
    for (const [k, v] of Object.entries(o)) if (v !== undefined && typeof v !== "function") out[snake(k)] = v;
    return out;
}

const nowUs = () => BigInt(Date.now()) * 1000n;

/* ---- handles ------------------------------------------------------------------------ */

type Kind = "publisher" | "subscriber" | "function_definition" | "remote_function" | "task_definition" |
            "remote_task" | "variable_definition" | "remote_variable";

/* What every handle has: a name, its match state, close(). */
export abstract class Handle {
    matchCount = 0;
    ready = false;
    closed = false;
    /** @internal */ value!: Codec;
    /** @internal */ req!: Codec;
    /** @internal */ prg!: Codec;
    /** @internal */ rsp!: Codec;
    /** @internal */ reflected = false;
    /** @internal */ reliable = true;
    /* the bridge keeps this handle's values as Rant wire bytes, see TopicOptions.raw */
    raw = false;

    /** @internal */
    constructor(readonly node: RantNode, readonly id: number, readonly name: string,
                /** @internal */ readonly kind: Kind, /** @internal */ readonly spec: Record<string, unknown>,
                /** @internal */ readonly shapes: Partial<Record<"value" | "req" | "prg" | "rsp", Shape>>) {}

    /** @internal */ retype(r: any): void {
        this.reliable = !!r.reliable;
        this.reflected = !!r.reflected;
        this.raw = !!r.raw;
        this.value = new Codec(r.schema, this.shapes.value, this.raw);
        this.req = new Codec(r.req, this.shapes.req, this.raw);
        this.prg = new Codec(r.prg, this.shapes.prg, this.raw);
        this.rsp = new Codec(r.rsp, this.shapes.rsp, this.raw);
    }
    /** @internal */ frame(_f: Frame, _payload: Uint8Array): void {}

    /* The schema in use: the one given, or the mesh's for a reflect handle. */
    get schema(): SchemaBlock | null { return this.value?.block ?? this.req?.block ?? null; }

    /* A reflect handle takes the mesh's types again when they moved. true when they changed. */
    async refresh(): Promise<boolean> {
        const r = await this.node.request({ op: "refresh", id: this.id });
        this.retype(r);
        return !!r.retyped;
    }

    async stats(): Promise<HandleStats> {
        const r = await this.node.request({ op: "stats" });
        const s = r.entities?.[String(this.id)] ?? {};
        return { queuedBytes: s.queued_bytes ?? 0, queuedMsgs: s.queued_msgs ?? 0, oldestAgeUs: s.oldest_age_us ?? 0,
                 dropped: s.dropped ?? 0, discarded: this.node.discarded(this.id) };
    }

    async close(): Promise<void> {
        if (this.closed) return;
        this.closed = true;
        this.node.forget(this);
        await this.node.request({ op: "close", id: this.id }).catch(() => {});
    }
    [Symbol.dispose](): void { void this.close(); }
}

export class Publisher<T> extends Handle {
    send(value: T): void { this.node.send(this, Op.Data, 0, 0, "", this.value.encode(value)); }
    async drain(timeoutMs = 1000): Promise<boolean> { return !!(await this.node.request({ op: "drain", id: this.id, timeout_ms: timeoutMs })).drained; }
}

export class Subscriber<T> extends Handle {
    /** @internal */ handler: ((value: T, msg: Message<T>) => void) | null = null;
    private waiting: Message<T>[] = [];
    private wakers: (() => void)[] = [];

    /** @internal */ frame(f: Frame, payload: Uint8Array): void {
        const value = this.value.decode(payload) as T;
        if (media.attach && this.value.media.length) media.attach(value, this, f.peer);
        const msg: Message<T> = { value, data: payload, publisher: f.peer, writtenUs: f.writtenUs, captureUs: f.captureUs, recvUs: Date.now() * 1000 };
        if (this.handler) { this.handler(value, msg); return; }
        this.waiting.push(msg);
        if (this.waiting.length > 1024) this.waiting.shift();
        for (const w of this.wakers.splice(0)) w();
    }
    /* A subscriber made without a handler is pulled: the oldest waiting message, or null. */
    async take(timeoutMs = 0): Promise<Message<T> | null> {
        if (!this.waiting.length && timeoutMs !== 0) await this.wait(timeoutMs);
        return this.waiting.shift() ?? null;
    }
    /* The newest waiting message, dropping the older ones, or null. */
    async latest(timeoutMs = 0): Promise<Message<T> | null> {
        if (!this.waiting.length && timeoutMs !== 0) await this.wait(timeoutMs);
        const m = this.waiting.pop() ?? null;
        this.waiting.length = 0;
        return m;
    }
    private wait(ms: number): Promise<void> {
        return new Promise((res) => {
            const t = ms > 0 ? setTimeout(res, ms) : null;
            this.wakers.push(() => { if (t) clearTimeout(t); res(); });
        });
    }
}

export class FunctionDefinition<Req, Rsp> extends Handle {
    /** @internal */ handler!: (value: Req, request: Request) => Rsp | Promise<Rsp>;
    /** @internal */ frame(f: Frame, payload: Uint8Array): void {
        if (f.op !== Op.Call) return;
        void (async () => {
            try {
                const out = await this.handler(this.req.decode(payload) as Req, { caller: f.peer, callerName: f.text, writtenUs: f.writtenUs });
                this.node.send(this, Op.Result, 0, f.seq, "", this.rsp.encode(out), true);
            } catch (e: any) {
                this.node.send(this, Op.Result, 1, f.seq, String(e?.message ?? e), new Uint8Array(0), true);
            }
        })();
    }
}

export class RemoteFunction<Req, Rsp> extends Handle {
    /* Never rejects on a status, only when the node closes. timeoutMs adds a bound of your own. */
    call(value: Req, opts: { timeoutMs?: number } = {}): Promise<Response<Rsp>> {
        return this.node.call(this, this.req.encode(value), opts.timeoutMs) as Promise<Response<Rsp>>;
    }
}

export class TaskDefinition<Req, Prg, Rsp> extends Handle {
    /** @internal */ handler!: (value: Req, ctx: TaskContext<Prg>) => Rsp | Promise<Rsp>;
    private aborts = new Map<number, AbortController>();
    /** @internal */ frame(f: Frame, payload: Uint8Array): void {
        if (f.op === Op.Cancel) { this.aborts.get(f.seq)?.abort(new CancelledError()); return; }
        if (f.op !== Op.Call) return;
        const ctrl = new AbortController();
        this.aborts.set(f.seq, ctrl);
        let done = false;
        const ctx: TaskContext<Prg> = {
            caller: f.peer, callerName: f.text, writtenUs: f.writtenUs, signal: ctrl.signal,
            progress: (v: Prg) => {
                if (done) throw new RantError("the task already finished");
                this.node.send(this, Op.Progress, 0, f.seq, "", this.prg.encode(v), true);
            },
        };
        void (async () => {
            let status = 0, text = "", out: Uint8Array = new Uint8Array(0);
            try { out = this.rsp.encode(await this.handler(this.req.decode(payload) as Req, ctx)); }
            catch (e: any) {
                const cancelled = e instanceof CancelledError || (ctrl.signal.aborted && e === ctrl.signal.reason);
                status = cancelled ? 5 : 1;
                text = String(e?.message ?? e);
                if (cancelled && e?.partial !== undefined) out = this.rsp.encode(e.partial);
            }
            done = true;
            this.aborts.delete(f.seq);
            this.node.send(this, Op.Result, status, f.seq, text, out, true);
        })();
    }
}

/* One task call in flight: result settles once, onProgress sees each update (null = RUNNING). */
export class TaskCall<Prg, Rsp> {
    private handler: ((value: Prg | null, info: { provider: number; writtenUs: bigint }) => void) | null = null;
    private early: [Prg | null, { provider: number; writtenUs: bigint }][] = [];
    /** @internal */ constructor(private task: RemoteTask<unknown, Prg, Rsp>, readonly callId: number, readonly result: Promise<Response<Rsp>>) {}
    onProgress(h: (value: Prg | null, info: { provider: number; writtenUs: bigint }) => void): this {
        this.handler = h;
        for (const [v, i] of this.early.splice(0)) h(v, i);
        return this;
    }
    async cancel(): Promise<CancelStatus> {
        return (await this.task.node.request({ op: "cancel", call: this.callId })).status as CancelStatus;
    }
    /** @internal */ progress(payload: Uint8Array, provider: number, writtenUs: bigint): void {
        const v = payload.length ? (this.task.prg.decode(payload) as Prg) : null;
        if (this.handler) this.handler(v, { provider, writtenUs }); else this.early.push([v, { provider, writtenUs }]);
    }
}

export class RemoteTask<Req, Prg, Rsp> extends Handle {
    call(value: Req): TaskCall<Prg, Rsp> { return this.node.callTask(this, this.req.encode(value)) as TaskCall<Prg, Rsp>; }
}

export class Variable<T> extends Handle {
    forced = false;
    private current: T | undefined;
    private changeHandler: ((value: T, info: { forced: boolean; source: number; writtenUs: bigint }) => void) | null = null;
    private writeHandler: ((value: T, info: { forced: boolean; source: number; writtenUs: bigint }) => void) | null = null;
    private wakers: (() => void)[] = [];

    get(): T | undefined { return this.current; }
    set(value: T): void { this.node.send(this, Op.Var, 0, 0, "", this.value.encode(value)); }
    force(value: T): void { this.node.send(this, Op.Var, 1, 0, "", this.value.encode(value)); }
    unforce(): void { this.node.send(this, Op.Var, 2, 0, "", new Uint8Array(0)); }
    /* Resolves true once a value exists, false at the timeout. */
    wait(timeoutMs = -1): Promise<boolean> {
        if (this.current !== undefined) return Promise.resolve(true);
        return new Promise((res) => {
            const t = timeoutMs >= 0 ? setTimeout(() => res(false), timeoutMs) : null;
            this.wakers.push(() => { if (t) clearTimeout(t); res(true); });
        });
    }
    /* Fires now with the current value, if one exists, then on every change. null clears. */
    onChange(h: ((value: T, info: { forced: boolean; source: number; writtenUs: bigint }) => void) | null): void {
        this.changeHandler = h;
        if (h && this.current !== undefined) h(this.current, { forced: this.forced, source: 0, writtenUs: 0n });
    }
    /* Every applied write, even one with the same value. Needs onWrite in the options. */
    onWrite(h: ((value: T, info: { forced: boolean; source: number; writtenUs: bigint }) => void) | null): void { this.writeHandler = h; }

    /** @internal */ frame(f: Frame, payload: Uint8Array): void {
        if (f.op !== Op.Var) return;
        const value = this.value.decode(payload) as T;
        if (media.attach && this.value.media.length) media.attach(value, this, f.peer);
        const info = { forced: (f.flags & 1) !== 0, source: f.peer, writtenUs: f.writtenUs };
        if (f.flags & 2) { this.writeHandler?.(value, info); return; }
        this.current = value;
        this.forced = info.forced;
        for (const w of this.wakers.splice(0)) w();
        this.changeHandler?.(value, info);
    }
}
export class VariableDefinition<T> extends Variable<T> {}
export class RemoteVariable<T> extends Variable<T> {}

/* ---- reflection --------------------------------------------------------------------- */

const toRtt = (r: any): Rtt => ({ us: r?.us ?? 0, jitterUs: r?.jitter_us ?? 0, samples: r?.samples ?? 0, atUs: r?.at_us ?? 0 });
const toEntity = (e: any): MeshEntity => ({
    kind: e.kind, name: e.name, provides: !!e.provides, consumes: !!e.consumes, reliable: !!e.reliable,
    providers: e.providers ?? 0, consumers: e.consumers ?? 0, provider: e.provider, from: e.from, generation: e.generation,
    writable: e.writable, forceable: e.forceable, cancellable: e.cancellable, exclusive: e.exclusive, multi: e.multi,
    incomplete: e.incomplete, conflict: e.conflict,
    schema: e.schema ? toBlock(e.schema) : undefined, rsp: e.rsp ? toBlock(e.rsp) : undefined, prg: e.prg ? toBlock(e.prg) : undefined,
});

export class Reflection {
    /** @internal */ constructor(private node: RantNode) {}
    /* Every discovered peer. rtt includes this client's hop to the bridge, meshRtt does not. */
    async peers(): Promise<Peer[]> {
        const r = await this.node.request({ op: "peers" });
        return r.peers.map((p: any) => ({ id: p.id, uuid: p.uuid ?? "", name: p.name, address: p.address, active: p.active,
            lastHeardUs: p.last_heard_us, epoch: p.epoch ?? 0, catchingUp: !!p.catching_up, fragmentSize: p.fragment_size ?? 0,
            rtt: toRtt(p.rtt), meshRtt: toRtt(p.mesh_rtt), rttMinUs: p.rtt_min_us ?? 0, missed: p.missed ?? 0 }));
    }
    /* What one node offers, 0 for this one. */
    async entities(peer = 0): Promise<MeshEntity[]> {
        const r = await this.node.request(peer ? { op: "peer_entities", peer } : { op: "entities" });
        return r.entities.map(toEntity);
    }
    /* The whole mesh, one entity per kind and name. */
    async mesh(): Promise<MeshEntity[]> { return (await this.node.request({ op: "mesh" })).entities.map(toEntity); }
    async find(kind: EntityKind, name: string): Promise<MeshEntity | null> {
        const r = await this.node.request({ op: "find", kind, name });
        return r.entity ? toEntity(r.entity) : null;
    }
    /* Moves whenever anything in the mesh changed: read the mesh again only when it moved. */
    async epoch(): Promise<number> { return (await this.node.request({ op: "epoch" })).epoch; }
    async meta(peer: number, sections = 0): Promise<{ valid: boolean; status: CallStatus; provider: number; info: Record<string, unknown> }> {
        const r = await this.node.request({ op: "meta", peer, sections });
        return { valid: !!r.valid, status: STATUS[r.status] ?? "cancelled", provider: r.provider ?? 0, info: r.info ?? {} };
    }
}

/* ---- the node ----------------------------------------------------------------------- */

type Pending = { resolve: (r: any) => void; reject: (e: Error) => void };
type Call = { resolve: (r: Response<unknown>) => void; codec: Codec; timer?: ReturnType<typeof setTimeout>; task?: TaskCall<unknown, unknown> };

export class RantNode {
    name = "";
    readonly reflection: Reflection;
    private ws: WebSocket | null = null;
    private token = "";
    private seq = 0;
    private nextId = 0;
    private nextCall = 0;
    private pending = new Map<number, Pending>();
    private handles = new Map<number, Handle>();
    private calls = new Map<number, Call>();
    private reasm = new Reassembly();
    private sender: Sender;
    private conn: Connection = { state: "connecting", carrier: "websocket", rtt: { us: 0, jitterUs: 0, samples: 0, atUs: 0 }, missed: 0 };
    private eventHandlers: ((e: RantEvent) => void)[] = [];
    private connHandlers: ((c: Connection) => void)[] = [];
    private logHandler: ((l: LogLine) => void) | null = null;
    private logLevels: string[] = [];
    private backoff = 250;
    private userClosed = false;
    private refused: RantError | null = null;     /* the bridge said no: retrying cannot help */
    /** @internal */ pc: RTCPeerConnection | null = null;
    /** @internal */ channels = new Map<number, RTCDataChannel>();
    /** @internal */ videoLines = new Map<RTCRtpTransceiver, { id: number; path: string; onVerdict: (v: string) => void }>();

    private constructor(readonly url: string, private opts: NodeOptions) {
        this.reflection = new Reflection(this);
        this.sender = new Sender((id, f) => this.carry(id, f));
        if (opts.onEvent) this.eventHandlers.push(opts.onEvent);
    }

    /* Connects, opens the node and tries WebRTC. Rejects when the bridge refuses. */
    static async connect(url: string, opts: NodeOptions = {}): Promise<RantNode> {
        const n = new RantNode(url, opts);
        try { await n.open(); }
        catch (e) { n.close(); throw e; }
        return n;
    }

    get connection(): Connection { return { ...this.conn, rtt: { ...this.conn.rtt } }; }
    onConnectionChange(h: (c: Connection) => void): void { this.connHandlers.push(h); }
    onEvent(h: (e: RantEvent) => void): void { this.eventHandlers.push(h); }

    /* ---- the connection ---- */

    private setState(state: ConnectionState, carrier = this.conn.carrier): void {
        if (this.conn.state === state && this.conn.carrier === carrier) return;
        this.conn.state = state;
        this.conn.carrier = carrier;
        for (const h of this.connHandlers) h(this.connection);
    }

    private emit(e: RantEvent): void {
        if (!this.eventHandlers.length && (e.kind === "error" || e.kind === "warning")) console.warn(`rant: ${e.text}`);
        for (const h of this.eventHandlers) h(e);
    }

    private async open(): Promise<void> {
        const resuming = !!this.token;
        const ws = new WebSocket(this.url);
        ws.binaryType = "arraybuffer";
        await new Promise<void>((res, rej) => {
            ws.onopen = () => res();
            ws.onerror = () => rej(new RantError(`cannot reach the bridge at ${this.url}`));
        });
        this.ws = ws;
        ws.onmessage = (m) => typeof m.data === "string" ? this.text(JSON.parse(m.data)) : this.binary(new Uint8Array(m.data as ArrayBuffer));
        ws.onclose = (e) => this.dropped(ws, e.code ? ` (${e.code}${e.reason ? " " + e.reason : ""})` : "");
        const { passphrase, name, onEvent: _e, transport: _t, rtc: _r, iceServers: _i, rtcTimeoutMs: _o, ...node } = this.opts;
        const r = await this.request({ op: "open", proto: PROTOCOL, passphrase, name, resume: this.token || undefined, ...wireOptions(node) })
            .catch((e: RantError & { answered?: boolean }) => { if (e.answered) this.refused = e; throw e; });
        this.token = r.resume_token;
        this.name = r.name;
        this.backoff = 250;
        this.setState("open", "websocket");
        if (resuming && r.resumed) this.emit({ kind: "resumed", text: "the bridge session resumed" });
        if (resuming && !r.resumed) await this.recreate();
        if (r.webrtc && this.opts.transport !== "websocket") await this.rtcConnect().catch(() => this.rtcDrop());
    }

    /* The bridge lost our session: make every handle again under its old id, with no old sends. */
    private async recreate(): Promise<void> {
        for (const [, c] of this.calls) { if (c.timer) clearTimeout(c.timer); c.resolve(this.failed("cancelled", "the bridge session was lost")); }
        this.calls.clear();
        for (const h of [...this.handles.values()].sort((a, b) => a.id - b.id)) {
            const r = await this.request({ op: "create", id: h.id, kind: h.kind, name: h.name, ...h.spec });
            h.retype(r);
        }
        if (this.logHandler) await this.request({ op: "log_subscribe", levels: this.logLevels });
        this.emit({ kind: "session_lost", text: "the bridge session was lost; every handle was made again" });
    }

    private dropped(ws: WebSocket, why = ""): void {
        if (this.ws !== ws) return;
        this.ws = null;
        this.rtcDrop();
        this.reasm.clear();
        this.sender.discardAll();
        for (const p of this.pending.values()) p.reject(new RantError("the connection to the bridge dropped" + why));
        this.pending.clear();
        if (this.userClosed || this.refused) {
            if (this.refused && !this.userClosed) this.emit({ kind: "error", text: `the bridge refused this node: ${this.refused.message}` });
            for (const [, c] of this.calls) { if (c.timer) clearTimeout(c.timer); c.resolve(this.failed("cancelled", "the node closed")); }
            this.calls.clear();
            this.setState("closed");
            return;
        }
        this.setState("reconnecting", "websocket");
        const retry = () => {
            if (this.userClosed) return;
            this.open().catch((e) => {
                this.emit({ kind: "error", text: `reconnecting failed: ${e?.message ?? e}` });
                this.backoff = Math.min(this.backoff * 2, 5000);
                setTimeout(retry, this.backoff);
            });
        };
        setTimeout(retry, this.backoff);
    }

    /* Closes the node. Calls still in flight answer cancelled. */
    close(): void {
        this.userClosed = true;
        if (this.ws) this.ws.close(); else this.setState("closed");
    }
    [Symbol.dispose](): void { this.close(); }

    /** @internal */ request(obj: Record<string, unknown>): Promise<any> {
        const ws = this.ws;
        if (!ws || ws.readyState !== WebSocket.OPEN) return Promise.reject(new RantError("not connected to the bridge"));
        const seq = ++this.seq;
        return new Promise((resolve, reject) => {
            this.pending.set(seq, { resolve, reject });
            ws.send(JSON.stringify({ ...obj, seq }));
        });
    }

    private text(m: any): void {
        switch (m.op) {
        case "reply": {
            const p = this.pending.get(m.seq);
            if (!p) return;
            this.pending.delete(m.seq);
            if (m.ok) p.resolve(m);
            else p.reject(Object.assign(new RantError(m.error ?? "refused"), { answered: true }));
            return;
        }
        case "event": {
            const { op: _op, event, peer_name, ...rest } = m;
            this.emit({ ...rest, kind: event, peerName: peer_name, text: m.text ?? event });
            return;
        }
        case "warning": this.emit({ kind: "warning", text: m.text, warning: m.kind, id: m.id }); return;
        case "match": {
            const h = this.handles.get(m.id);
            if (h) { h.matchCount = m.count; h.ready = !!m.ready; }
            return;
        }
        case "log":
            this.logHandler?.({ level: m.level, node: m.node, text: m.text, wallUs: m.wall_us, monoUs: m.mono_us, recvUs: m.recv_us, writtenUs: m.written_us });
            return;
        case "rtc":
            if (this.pc && m.candidate) this.pc.addIceCandidate({ candidate: m.candidate, sdpMid: m.mid }).catch(() => {});
            return;
        }
    }

    private binary(b: Uint8Array): void {
        const f = parseFrame(b);
        if (!f) return;
        if (f.op === Op.Ping) { this.ping(f); return; }
        const whole = this.reasm.add(f);
        if (!whole) return;
        if (f.op === Op.Result || f.op === Op.Progress) {
            const c = this.calls.get(f.seq);
            if (!c) return;
            if (f.op === Op.Progress) { c.task?.progress(whole, f.peer, f.writtenUs); return; }
            this.calls.delete(f.seq);
            if (c.timer) clearTimeout(c.timer);
            c.resolve(this.response(f, whole, c.codec));
            return;
        }
        try { this.handles.get(f.id)?.frame(f, whole); }
        catch (e: any) { this.emit({ kind: "error", id: f.id, text: `a message did not decode: ${e?.message ?? e}` }); }
    }

    /* The bridge measures the link with pings and tells us what it saw. */
    private ping(f: Frame): void {
        if (f.payload.length) {
            try {
                const s = decode(f.payload) as any;
                this.conn.rtt = { us: s.us ?? 0, jitterUs: s.jitter_us ?? 0, samples: s.samples ?? 0, atUs: Date.now() * 1000 };
                this.conn.missed = s.missed ?? 0;
            } catch { /* an old bridge sends no stats */ }
        }
        const pong = buildPong(f);
        if (!this.carry(0, pong)) this.ws?.send(pong);
    }

    /* The entity's data channel when it is open and has room, else the WebSocket. */
    private carry(id: number, frame: Uint8Array): boolean {
        const dc = this.channels.get(id);
        if (dc && dc.readyState === "open" && dc.bufferedAmount < 65536 &&
            frame.length <= ((this.pc as any)?.sctp?.maxMessageSize ?? 65536)) { dc.send(frame as any); return true; }
        const ws = this.ws;
        if (!ws || ws.readyState !== WebSocket.OPEN || ws.bufferedAmount > 65536) return false;
        ws.send(frame);
        return true;
    }

    /** @internal */ send(h: Handle, op: Op, flags: number, seq: number, text: string, payload: Uint8Array, reply = false): void {
        if (h.closed) throw new RantError(`'${h.name}' is closed`);
        if (this.conn.state !== "open") {
            /* nothing waits for a reconnect: a late command is worse than none */
            this.sender.discarded.set(h.id, (this.sender.discarded.get(h.id) ?? 0) + 1);
            if (!reply) this.emit({ kind: "discarded", id: h.id, text: `'${h.name}' is not connected, the send was dropped` });
            return;
        }
        this.sender.push({ op, flags, id: h.id, seq, peer: 0, writtenUs: 0n, captureUs: op === Op.Data || op === Op.Var ? nowUs() : 0n, text, payload });
    }

    /** @internal */ discarded(id: number): number { return this.sender.discarded.get(id) ?? 0; }

    private failed(status: CallStatus, message: string): Response<unknown> {
        return { ok: false, status, value: undefined, hasValue: false, data: new Uint8Array(0), message, provider: 0, writtenUs: 0n };
    }

    private response(f: Frame, payload: Uint8Array, codec: Codec): Response<unknown> {
        const status = STATUS[f.flags] ?? "cancelled";
        let value: unknown;
        if (payload.length) { try { value = codec.decode(payload); } catch { value = undefined; } }
        return { ok: status === "ok", status, value, hasValue: value !== undefined, data: payload,
                 message: f.text || STATUS_TEXT[status], provider: f.peer, writtenUs: f.writtenUs };
    }

    /** @internal */ call(h: Handle, payload: Uint8Array, timeoutMs?: number): Promise<Response<unknown>> {
        const id = ++this.nextCall;
        return new Promise((resolve) => {
            const c: Call = { resolve, codec: h.rsp };
            if (timeoutMs && timeoutMs > 0) c.timer = setTimeout(() => { this.calls.delete(id); resolve(this.failed("timeout", "timeout")); }, timeoutMs);
            if (this.conn.state !== "open") { resolve(this.failed("cancelled", "not connected to the bridge")); return; }
            this.calls.set(id, c);
            this.send(h, Op.Call, 0, id, "", payload);
        });
    }

    /** @internal */ callTask(h: RemoteTask<any, any, any>, payload: Uint8Array): TaskCall<unknown, unknown> {
        const id = ++this.nextCall;
        let resolve!: (r: Response<unknown>) => void;
        const result = new Promise<Response<unknown>>((r) => { resolve = r; });
        const call = new TaskCall<unknown, unknown>(h, id, result);
        if (this.conn.state !== "open") resolve(this.failed("cancelled", "not connected to the bridge"));
        else { this.calls.set(id, { resolve, codec: h.rsp, task: call }); this.send(h, Op.Call, 0, id, "", payload); }
        return call;
    }

    /** @internal */ forget(h: Handle): void {
        this.handles.delete(h.id);
        this.channels.get(h.id)?.close();
        this.channels.delete(h.id);
    }

    /* ---- making handles ---- */

    private async create<H extends Handle>(make: (id: number) => H, fields: Record<string, unknown>): Promise<H> {
        const id = ++this.nextId;
        if (id > 0xfffe) throw new RantError("out of entity ids");
        const h = make(id);
        const r = await this.request({ op: "create", id, kind: h.kind, name: h.name, ...h.spec, ...fields });
        h.retype(r);
        this.handles.set(id, h);
        if (this.pc) this.openChannel(id, h.reliable);
        return h;
    }

    private schemaFields(keys: Record<string, Schema<any> | string | null | undefined>): { fields: Record<string, string>; shapes: Record<string, Shape> } {
        const fields: Record<string, string> = {}, shapes: Record<string, Shape> = {};
        for (const [k, s] of Object.entries(keys)) {
            if (s === null || s === undefined) continue;
            fields[k === "value" ? "schema" : k] = dsl(s);
            if (typeof s !== "string") shapes[k] = s.shape;
        }
        return { fields, shapes };
    }

    async publisher<S extends Schema<any> | string | null>(name: string, schema: S, opts: TopicOptions = {}): Promise<Publisher<S extends Schema<any> ? Infer<S> : unknown>> {
        const { fields, shapes } = this.schemaFields({ value: schema });
        return this.create((id) => new Publisher<any>(this, id, name, "publisher", { ...fields, ...wireOptions(opts) }, shapes), {});
    }

    /* A subscriber without a handler is pulled with take() and latest(). */
    async subscriber<S extends Schema<any> | string | null>(name: string, schema: S,
            handler?: ((value: S extends Schema<any> ? Infer<S> : unknown, msg: Message<S extends Schema<any> ? Infer<S> : unknown>) => void) | null,
            opts: TopicOptions = {}): Promise<Subscriber<S extends Schema<any> ? Infer<S> : unknown>> {
        const { fields, shapes } = this.schemaFields({ value: schema });
        const s = await this.create((id) => new Subscriber<any>(this, id, name, "subscriber", { ...fields, ...wireOptions(opts) }, shapes), {});
        s.handler = handler ?? null;
        return s;
    }

    async functionDefinition<Q extends Schema<any> | string | null, P extends Schema<any> | string | null>(
            name: string, handler: (value: Val<Q>, request: Request) => Val<P> | Promise<Val<P>>, req: Q, rsp: P,
            opts: FunctionOptions = {}): Promise<FunctionDefinition<Val<Q>, Val<P>>> {
        const { fields, shapes } = this.schemaFields({ req, rsp });
        return this.create((id) => {
            const h = new FunctionDefinition<any, any>(this, id, name, "function_definition", { ...fields, ...wireOptions(opts) }, shapes);
            h.handler = handler;
            return h;
        }, {});
    }

    async remoteFunction<Q extends Schema<any> | string | null, P extends Schema<any> | string | null>(
            name: string, req: Q, rsp: P, opts: FunctionOptions = {}): Promise<RemoteFunction<Val<Q>, Val<P>>> {
        const { fields, shapes } = this.schemaFields({ req, rsp });
        return this.create((id) => new RemoteFunction<any, any>(this, id, name, "remote_function", { ...fields, ...wireOptions(opts) }, shapes), {});
    }

    async taskDefinition<Q extends Schema<any> | string | null, G extends Schema<any> | string | null, P extends Schema<any> | string | null>(
            name: string, handler: (value: Val<Q>, ctx: TaskContext<Val<G>>) => Val<P> | Promise<Val<P>>, req: Q, prg: G, rsp: P,
            opts: TaskOptions = {}): Promise<TaskDefinition<Val<Q>, Val<G>, Val<P>>> {
        const { fields, shapes } = this.schemaFields({ req, prg, rsp });
        return this.create((id) => {
            const h = new TaskDefinition<any, any, any>(this, id, name, "task_definition", { ...fields, ...wireOptions(opts) }, shapes);
            h.handler = handler;
            return h;
        }, {});
    }

    async remoteTask<Q extends Schema<any> | string | null, G extends Schema<any> | string | null, P extends Schema<any> | string | null>(
            name: string, req: Q, prg: G, rsp: P, opts: TaskOptions = {}): Promise<RemoteTask<Val<Q>, Val<G>, Val<P>>> {
        const { fields, shapes } = this.schemaFields({ req, prg, rsp });
        return this.create((id) => new RemoteTask<any, any, any>(this, id, name, "remote_task", { ...fields, ...wireOptions(opts) }, shapes), {});
    }

    async variableDefinition<S extends Schema<any> | string | null>(name: string, schema: S, opts: VariableOptions<Val<S>> = {}): Promise<VariableDefinition<Val<S>>> {
        const { initial, ...rest } = opts;
        const { fields, shapes } = this.schemaFields({ value: schema });
        const v = await this.create((id) => new VariableDefinition<any>(this, id, name, "variable_definition", { ...fields, ...wireOptions(rest) }, shapes), {});
        if (initial !== undefined) v.set(initial);
        return v;
    }

    async remoteVariable<S extends Schema<any> | string | null>(name: string, schema: S, opts: VariableOptions<Val<S>> = {}): Promise<RemoteVariable<Val<S>>> {
        const { fields, shapes } = this.schemaFields({ value: schema });
        return this.create((id) => new RemoteVariable<any>(this, id, name, "remote_variable", { ...fields, ...wireOptions(opts) }, shapes), {});
    }

    /* ---- the rest of the node ---- */

    /* Waits until discovery and matching settle for everything made so far. */
    async settle(timeoutMs = -1): Promise<boolean> { return !!(await this.request({ op: "settle", timeout_ms: timeoutMs })).settled; }

    async log(level: "error" | "warn" | "info", text: string): Promise<void> { await this.request({ op: "log", level, text }); }

    /* Every other node's log lines. One handler, a later call replaces it. */
    async onLog(handler: (line: LogLine) => void, levels: ("error" | "warn" | "info")[] = ["error", "warn", "info"]): Promise<void> {
        this.logHandler = handler;
        this.logLevels = levels;
        await this.request({ op: "log_subscribe", levels });
    }

    /* ---- WebRTC ---- */

    private async rtcConnect(): Promise<void> {
        const Pc = this.opts.rtc ?? (globalThis as any).RTCPeerConnection;
        if (!Pc) return;
        const r = await this.request({ op: "rtc" });
        const pc: RTCPeerConnection = new Pc({ iceServers: [...(r.ice_servers ?? []), ...(this.opts.iceServers ?? [])] });
        this.pc = pc;
        pc.onicecandidate = (e) => {
            if (e.candidate?.candidate) this.request({ op: "rtc", candidate: e.candidate.candidate, mid: e.candidate.sdpMid ?? "" }).catch(() => {});
        };
        this.openChannel(0, true);
        for (const h of this.handles.values()) this.openChannel(h.id, h.reliable);
        const up = new Promise<void>((res, rej) => {
            const t = setTimeout(() => rej(new Error("webrtc did not connect")), this.opts.rtcTimeoutMs ?? 4000);
            pc.onconnectionstatechange = () => {
                const s = pc.connectionState;
                if (s === "connected") { clearTimeout(t); this.setState("open", "webrtc"); res(); }
                else if (s === "failed" || s === "closed") { clearTimeout(t); this.rtcDrop(); rej(new Error("webrtc " + s)); }
                else if (s === "disconnected") this.setState(this.conn.state, "websocket");
            };
        });
        await this.offer();
        await up;
    }

    /** @internal */ async offer(): Promise<void> {
        const pc = this.pc;
        if (!pc) return;
        const o = await pc.createOffer();
        await pc.setLocalDescription(o);
        const tracks: Record<string, unknown> = {}, lines = new Map<string, (v: string) => void>();
        for (const [t, l] of this.videoLines)
            if (t.mid) { tracks[t.mid] = { id: l.id, path: l.path }; lines.set(t.mid, l.onVerdict); }
        const r = await this.request({ op: "rtc", sdp: pc.localDescription?.sdp ?? o.sdp, tracks });
        await pc.setRemoteDescription({ type: "answer", sdp: r.sdp });
        for (const [mid, cb] of lines) cb(r.tracks?.[mid] ?? "no answer");
    }

    private openChannel(id: number, reliable: boolean): void {
        const pc = this.pc;
        if (!pc || this.channels.has(id)) return;
        const dc = pc.createDataChannel(`d${id}`, reliable ? { negotiated: true, id, ordered: true }
                                                          : { negotiated: true, id, ordered: false, maxRetransmits: 0 });
        dc.binaryType = "arraybuffer";
        dc.bufferedAmountLowThreshold = 65536;
        dc.onbufferedamountlow = () => this.sender.pump();
        dc.onmessage = (e) => this.binary(new Uint8Array(e.data as ArrayBuffer));
        this.channels.set(id, dc);
    }

    private rtcDrop(): void {
        const pc = this.pc;
        this.pc = null;
        this.channels.clear();
        this.videoLines.clear();
        if (pc) { pc.onconnectionstatechange = null; pc.onicecandidate = null; try { pc.close(); } catch { /* already closed */ } }
        if (this.conn.state === "open") this.setState("open", "websocket");
    }
}

type Val<S> = S extends Schema<any> ? Infer<S> : unknown;

function buildPong(f: Frame): Uint8Array {
    return buildFrame({ ...f, op: Op.Pong, payload: new Uint8Array(0), chunk: undefined });
}
