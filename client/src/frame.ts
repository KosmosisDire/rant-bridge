/* The data plane frame, the same header both ways (PROTOCOL.md):
 * [u8 op][u8 flags][u16 id][u32 seq][u32 peer][u64 written_us][u64 capture_us][u8 text_len][text]
 * [u16 msg][u32 total][u32 offset] when flags bit 7 marks a chunk, then the payload. */

export const enum Op { Data = 1, Var = 2, Call = 3, Result = 4, Progress = 5, Cancel = 6, Ping = 7, Pong = 8 }

const CHUNK = 0x80;
const HEADER = 29;
const enc = new TextEncoder();
const dec = new TextDecoder();

export interface Frame {
    op: number; flags: number; id: number; seq: number; peer: number;
    writtenUs: bigint; captureUs: bigint; text: string;
    chunk?: { msg: number; total: number; offset: number };
    payload: Uint8Array;
}

export function buildFrame(f: Frame): Uint8Array {
    let t = f.text ? enc.encode(f.text) : new Uint8Array(0);
    if (t.length > 255) t = t.subarray(0, 255);
    const extra = f.chunk ? 10 : 0;
    const out = new Uint8Array(HEADER + t.length + extra + f.payload.length);
    const v = new DataView(out.buffer);
    out[0] = f.op;
    out[1] = f.flags | (f.chunk ? CHUNK : 0);
    v.setUint16(2, f.id, true); v.setUint32(4, f.seq, true); v.setUint32(8, f.peer, true);
    v.setBigUint64(12, f.writtenUs, true); v.setBigUint64(20, f.captureUs, true);
    out[28] = t.length;
    out.set(t, HEADER);
    let at = HEADER + t.length;
    if (f.chunk) {
        v.setUint16(at, f.chunk.msg, true); v.setUint32(at + 2, f.chunk.total, true); v.setUint32(at + 6, f.chunk.offset, true);
        at += 10;
    }
    out.set(f.payload, at);
    return out;
}

export function parseFrame(b: Uint8Array): Frame | null {
    if (b.length < HEADER) return null;
    const tl = b[28];
    let at = HEADER + tl;
    if (b.length < at) return null;
    const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
    const f: Frame = {
        op: b[0], flags: b[1] & ~CHUNK, id: v.getUint16(2, true), seq: v.getUint32(4, true), peer: v.getUint32(8, true),
        writtenUs: v.getBigUint64(12, true), captureUs: v.getBigUint64(20, true),
        text: tl ? dec.decode(b.subarray(HEADER, at)) : "", payload: b,
    };
    if (b[1] & CHUNK) {
        if (b.length < at + 10) return null;
        f.chunk = { msg: v.getUint16(at, true), total: v.getUint32(at + 2, true), offset: v.getUint32(at + 6, true) };
        at += 10;
    }
    f.payload = b.subarray(at);
    return f;
}

/* Joins chunks per entity. A newer message drops an unfinished older one. */
export class Reassembly {
    private parts = new Map<number, { msg: number; bytes: Uint8Array; got: number }>();
    add(f: Frame): Uint8Array | null {
        if (!f.chunk) return f.payload;
        const { msg, total, offset } = f.chunk;
        if (offset + f.payload.length > total) return null;
        let p = this.parts.get(f.id);
        if (!p || p.msg !== msg) { p = { msg, bytes: new Uint8Array(total), got: 0 }; this.parts.set(f.id, p); }
        p.bytes.set(f.payload, offset);
        p.got += f.payload.length;
        if (p.got < total) return null;
        this.parts.delete(f.id);
        return p.bytes;
    }
    clear(): void { this.parts.clear(); }
}

export const CHUNK_BYTES = 16 * 1024;

/* Everything the client sends as data: one lane per entity, a chunk at a time in round robin,
 * so a large message never holds up a small one for longer than one chunk. */
export class Sender {
    private lanes = new Map<number, { q: Frame[]; offset: number; msg: number }>();
    private cursor = 0;
    private timer: ReturnType<typeof setTimeout> | null = null;
    discarded = new Map<number, number>();

    constructor(private carrier: (id: number, frame: Uint8Array) => boolean) {}

    push(f: Frame): void {
        let l = this.lanes.get(f.id);
        if (!l) { l = { q: [], offset: 0, msg: 0 }; this.lanes.set(f.id, l); }
        l.q.push(f);
        this.pump();
    }

    /* At a drop nothing old may reach a robot later: everything waiting goes, counted per entity. */
    discardAll(): void {
        for (const [id, l] of this.lanes) {
            if (l.q.length) this.discarded.set(id, (this.discarded.get(id) ?? 0) + l.q.length);
        }
        this.lanes.clear();
        if (this.timer) { clearTimeout(this.timer); this.timer = null; }
    }

    pump(): void {
        for (;;) {
            const ids = [...this.lanes.keys()].filter((id) => this.lanes.get(id)!.q.length).sort((a, b) => a - b);
            if (!ids.length) return;
            const start = ids.findIndex((id) => id > this.cursor);
            const order = start < 0 ? ids : [...ids.slice(start), ...ids.slice(0, start)];
            let sent = false, blocked = false;
            for (const id of order) {
                const l = this.lanes.get(id)!, f = l.q[0], total = f.payload.length;
                const whole = total <= CHUNK_BYTES && l.offset === 0;
                const n = whole ? total : Math.min(CHUNK_BYTES, total - l.offset);
                const frame = buildFrame(whole ? f : { ...f, chunk: { msg: l.msg, total, offset: l.offset },
                                                       payload: f.payload.subarray(l.offset, l.offset + n) });
                if (!this.carrier(id, frame)) { blocked = true; continue; }
                sent = true;
                this.cursor = id;
                l.offset += n;
                if (l.offset >= total) { l.q.shift(); l.offset = 0; l.msg = (l.msg + 1) & 0xffff; }
            }
            if (!sent) {
                /* a full carrier has no drain event on a WebSocket, so look again shortly */
                if (blocked && !this.timer) this.timer = setTimeout(() => { this.timer = null; this.pump(); }, 2);
                return;
            }
        }
    }
}
