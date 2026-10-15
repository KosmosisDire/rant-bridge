/* The CBOR subset the bridge speaks (RFC 8949, typed arrays from RFC 8746). The bridge
 * writes 64 bit fields in their 8 byte form, so they always decode as bigint here. */

const enc = new TextEncoder();
const dec = new TextDecoder();

type Typed = Uint16Array | Uint32Array | BigUint64Array | Int8Array | Int16Array | Int32Array |
             BigInt64Array | Float32Array | Float64Array;

const TAGS: [number, new (b: ArrayBuffer) => Typed][] = [
    [69, Uint16Array], [70, Uint32Array], [71, BigUint64Array], [72, Int8Array], [77, Int16Array],
    [78, Int32Array], [79, BigInt64Array], [85, Float32Array], [86, Float64Array],
];
const TAG_OF = new Map<Function, number>(TAGS.map(([t, c]) => [c, t]));
const CTOR_OF = new Map<number, new (b: ArrayBuffer) => Typed>(TAGS);

class Out {
    buf = new Uint8Array(256);
    view = new DataView(this.buf.buffer);
    n = 0;
    room(k: number): void {
        if (this.n + k <= this.buf.length) return;
        let cap = this.buf.length * 2;
        while (cap < this.n + k) cap *= 2;
        const b = new Uint8Array(cap);
        b.set(this.buf.subarray(0, this.n));
        this.buf = b;
        this.view = new DataView(b.buffer);
    }
    head(major: number, v: number): void {
        this.room(9);
        const m = major << 5;
        if (v < 24) this.buf[this.n++] = m | v;
        else if (v < 0x100) { this.buf[this.n++] = m | 24; this.buf[this.n++] = v; }
        else if (v < 0x10000) { this.buf[this.n++] = m | 25; this.view.setUint16(this.n, v); this.n += 2; }
        else if (v < 0x100000000) { this.buf[this.n++] = m | 26; this.view.setUint32(this.n, v); this.n += 4; }
        else { this.buf[this.n++] = m | 27; this.view.setBigUint64(this.n, BigInt(v)); this.n += 8; }
    }
    bytes(b: Uint8Array): void { this.head(2, b.length); this.room(b.length); this.buf.set(b, this.n); this.n += b.length; }
}

function write(o: Out, v: unknown): void {
    if (v === null || v === undefined) { o.room(1); o.buf[o.n++] = 0xf6; return; }
    switch (typeof v) {
    case "boolean": o.room(1); o.buf[o.n++] = v ? 0xf5 : 0xf4; return;
    case "number":                 /* always a double: the bridge converts to the field's type */
        o.room(9); o.buf[o.n++] = 0xfb; o.view.setFloat64(o.n, v); o.n += 8; return;
    case "bigint":
        o.room(9);
        if (v < 0n) { o.buf[o.n++] = 0x3b; o.view.setBigUint64(o.n, -1n - v); }
        else { o.buf[o.n++] = 0x1b; o.view.setBigUint64(o.n, v); }
        o.n += 8;
        return;
    case "string": { const b = enc.encode(v); o.head(3, b.length); o.room(b.length); o.buf.set(b, o.n); o.n += b.length; return; }
    }
    if (v instanceof Uint8Array) { o.bytes(v); return; }
    if (ArrayBuffer.isView(v)) {
        const tag = TAG_OF.get(v.constructor);
        if (tag === undefined) throw new TypeError("cannot send a " + v.constructor.name);
        o.head(6, tag);
        o.bytes(new Uint8Array(v.buffer, v.byteOffset, v.byteLength));
        return;
    }
    if (Array.isArray(v)) { o.head(4, v.length); for (const e of v) write(o, e); return; }
    if (typeof v === "object") {
        const entries = Object.entries(v as Record<string, unknown>).filter(([, x]) => x !== undefined);
        o.head(5, entries.length);
        for (const [k, x] of entries) { write(o, k); write(o, x); }
        return;
    }
    throw new TypeError("cannot send a " + typeof v);
}

export function encode(v: unknown): Uint8Array {
    const o = new Out();
    write(o, v);
    return o.buf.subarray(0, o.n);
}

/* map keys repeat in every message, so short ones are decoded once */
const keyCache = new Map<number, string>();

export function decode(u8: Uint8Array): unknown {
    const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
    let i = 0;
    const need = (k: number) => { if (i + k > u8.length) throw new Error("cbor: truncated"); };
    const arg = (ai: number): number | bigint => {
        if (ai < 24) return ai;
        if (ai === 24) { need(1); return u8[i++]; }
        if (ai === 25) { need(2); i += 2; return dv.getUint16(i - 2); }
        if (ai === 26) { need(4); i += 4; return dv.getUint32(i - 4); }
        if (ai === 27) { need(8); i += 8; return dv.getBigUint64(i - 8); }
        throw new Error("cbor: unsupported length");
    };
    const count = (ai: number): number => { const n = arg(ai); return typeof n === "bigint" ? Number(n) : n; };
    const key = (n: number): string => {
        need(n);
        if (n <= 12) {
            let h = n;
            for (let k = 0; k < n; k++) h = h * 257 + u8[i + k];
            let s = keyCache.get(h);
            if (s === undefined) { s = dec.decode(u8.subarray(i, i + n)); keyCache.set(h, s); }
            i += n;
            return s;
        }
        const s = dec.decode(u8.subarray(i, i + n)); i += n; return s;
    };
    const item = (depth: number): unknown => {
        if (depth > 64) throw new Error("cbor: nested too deep");
        need(1);
        const b = u8[i++], m = b >> 5, ai = b & 31;
        switch (m) {
        case 0: return arg(ai);
        case 1: { const v = arg(ai); return typeof v === "bigint" ? -1n - v : -1 - v; }
        case 2: { const n = count(ai); need(n); i += n; return u8.subarray(i - n, i); }
        case 3: { const n = count(ai); need(n); const s = dec.decode(u8.subarray(i, i + n)); i += n; return s; }
        case 4: { const n = count(ai), a = new Array(n); for (let k = 0; k < n; k++) a[k] = item(depth + 1); return a; }
        case 5: {
            const n = count(ai), o: Record<string, unknown> = {};
            for (let k = 0; k < n; k++) {
                need(1);
                const kb = u8[i++];
                if (kb >> 5 !== 3) throw new Error("cbor: map keys must be text");
                o[key(count(kb & 31))] = item(depth + 1);
            }
            return o;
        }
        case 6: {
            const tag = count(ai), v = item(depth + 1), Ctor = CTOR_OF.get(tag);
            if (!Ctor || !(v instanceof Uint8Array)) return v;
            return new Ctor(v.slice().buffer);      /* a copy: element alignment is not guaranteed */
        }
        default:
            if (ai === 20) return false;
            if (ai === 21) return true;
            if (ai === 22 || ai === 23) return null;
            if (ai === 25) { need(2); i += 2; return half(dv.getUint16(i - 2)); }
            if (ai === 26) { need(4); i += 4; return dv.getFloat32(i - 4); }
            if (ai === 27) { need(8); i += 8; return dv.getFloat64(i - 8); }
            throw new Error("cbor: unsupported simple value");
        }
    };
    const v = item(0);
    if (i !== u8.length) throw new Error("cbor: trailing bytes");
    return v;
}

function half(h: number): number {
    const e = (h >> 10) & 31, m = h & 1023;
    const v = e === 0 ? m * 2 ** -24 : e === 31 ? (m ? NaN : Infinity) : (m + 1024) * 2 ** (e - 25);
    return h & 0x8000 ? -v : v;
}
