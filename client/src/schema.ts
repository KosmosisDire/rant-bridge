/* Schemas as values: each one spells its DSL text for the bridge and carries its TS type,
 * so a handle made from it is typed without generics written by hand. */

export type Scalar = "u8" | "u16" | "u32" | "u64" | "i8" | "i16" | "i32" | "i64" | "f32" | "f64" | "bool";

export type Shape =
    | { k: "scalar"; t: Scalar }
    | { k: "string"; cap?: number }
    | { k: "array"; elem: Shape; n?: number }
    | { k: "enum"; backing: Scalar; options: Record<string, number> }
    | { k: "map" }
    | { k: "struct"; name: string; fields: Record<string, Shape> }
    | { k: "named"; name: string };            /* a standard type, in scope by name */

declare const typeTag: unique symbol;

/* A schema: its shape, and the TS type of its values as a phantom. */
export interface Schema<T = unknown> {
    readonly shape: Shape;
    readonly [typeTag]?: T;
}

/* The TS type of a schema's values. */
export type Infer<S> = S extends Schema<infer T> ? T : never;

const make = <T>(shape: Shape): Schema<T> => ({ shape }) as Schema<T>;

/* A scalar keeps its kind in its type, so an array of it knows which typed array it is. */
type ScalarSchema<T, K extends Scalar> = Schema<T> & { readonly shape: { k: "scalar"; t: K } };
const scalar = <T, K extends Scalar>(t: K) => make<T>({ k: "scalar", t }) as ScalarSchema<T, K>;

export const u8 = scalar<number, "u8">("u8");
export const u16 = scalar<number, "u16">("u16");
export const u32 = scalar<number, "u32">("u32");
export const u64 = scalar<bigint, "u64">("u64");
export const i8 = scalar<number, "i8">("i8");
export const i16 = scalar<number, "i16">("i16");
export const i32 = scalar<number, "i32">("i32");
export const i64 = scalar<bigint, "i64">("i64");
export const f32 = scalar<number, "f32">("f32");
export const f64 = scalar<number, "f64">("f64");
export const bool = scalar<boolean, "bool">("bool");
export const map = make<Record<string, unknown>>({ k: "map" });

/* A string of at most cap bytes, or of any length without one. */
export function string(cap?: number): Schema<string> { return make<string>({ k: "string", cap }); }

/* The typed array each numeric element kind arrives as. Booleans arrive as boolean[]. */
type TypedFor = {
    u8: Uint8Array; u16: Uint16Array; u32: Uint32Array; u64: BigUint64Array; i8: Int8Array;
    i16: Int16Array; i32: Int32Array; i64: BigInt64Array; f32: Float32Array; f64: Float64Array;
};

/* An array of n elements, or of any length without n. Numeric elements arrive as typed arrays. */
export function array<S extends Schema<any>>(elem: S, n?: number):
    Schema<S extends { shape: { k: "scalar"; t: infer K extends keyof TypedFor } } ? TypedFor[K] : Infer<S>[]> {
    return make({ k: "array", elem: elem.shape, n });
}

/* An integer with named values, read and written as the names. */
export function enumOf<const O extends Record<string, number>>(options: O, backing: Schema<number> = u8): Schema<keyof O & string> {
    const s = backing.shape;
    if (s.k !== "scalar") throw new TypeError("an enum is backed by an integer");
    return make({ k: "enum", backing: s.t, options });
}

/* A named struct. Member names are yours; the wire spells them in snake_case. */
export function struct<const F extends Record<string, Schema<any>>>(name: string, fields: F):
    Schema<{ -readonly [K in keyof F]: Infer<F[K]> }> {
    const shapes: Record<string, Shape> = {};
    for (const [k, v] of Object.entries(fields)) shapes[k] = v.shape;
    return make({ k: "struct", name, fields: shapes });
}

/* DSL text for a schema: named structs as leading definitions, the root last. */
export function dsl(s: Schema<any> | string): string {
    if (typeof s === "string") return s;
    const defs = new Map<string, string>();
    const spell = (sh: Shape): string => {
        switch (sh.k) {
        case "scalar": return sh.t;
        case "string": return sh.cap === undefined ? "string" : `string<${sh.cap}>`;
        case "array": return `${spell(sh.elem)}[${sh.n ?? ""}]`;
        case "enum": return `enum<${sh.backing}> { ${Object.entries(sh.options).map(([k, v]) => `${k} = ${v}`).join(", ")} }`;
        case "map": return "map";
        case "named": return sh.name;
        case "struct": {
            if (!defs.has(sh.name)) {
                defs.set(sh.name, "");            /* reserve the slot before members, so order stays dependency first */
                const body = Object.entries(sh.fields).map(([k, f]) => `${k}: ${spell(f)}`).join(", ");
                defs.delete(sh.name);
                defs.set(sh.name, `${sh.name} { ${body} }`);
            }
            return sh.name;
        }
        }
    };
    const root = spell(s.shape);
    if (s.shape.k === "struct") return [...defs.values()].join("\n");
    return [...defs.values(), root].join("\n");
}

/* Media fields carry decode() on their values once rant/video is imported. */
export interface VideoFrameValue {
    codec: "Unknown" | "Mjpeg" | "H264" | "H265" | "Av1";
    width: number; height: number; keyframe: boolean; pts: bigint; data: Uint8Array;
    /* The decoded picture, closed for you when this stream's next frame decodes. null until a keyframe. */
    decode(): Promise<VideoFrame | null>;
}
export interface ImageValue {
    width: number; height: number; stride: number;
    format: "Mono8" | "Mono16" | "Rgb8" | "Rgba8" | "Bgr8" | "Yuyv" | "Nv12" | "Monof32" | "Jpeg" | "Png";
    data: Uint8Array;
    decode(): Promise<ImageBitmap>;
}
export interface ExternalVideoStreamValue {
    kind: "Rtsp" | "WebrtcWhep" | "Hls" | "Srt" | "Rtp" | "HttpMjpeg" | "Other";
    codec: "Unknown" | "Mjpeg" | "H264" | "H265" | "Av1";
    width: number; height: number; url: string; name: string;
}

const named = <T>(name: string) => make<T>({ k: "named", name });
type D2 = { x: number; y: number };
type D3 = { x: number; y: number; z: number };
type D4 = { x: number; y: number; z: number; w: number };
type Seg = { a: D3; b: D3 };
type Seg2 = { a: D2; b: D2 };
type PoseT = { position: D3; orientation: D4 };
type Pose2T = { position: D2; angle: number };

/* The standard types of docs/stdtypes.md, by name. */
export const types = {
    Float2: named<D2>("Float2"), Float3: named<D3>("Float3"), Float4: named<D4>("Float4"),
    Double2: named<D2>("Double2"), Double3: named<D3>("Double3"), Double4: named<D4>("Double4"),
    Int2: named<D2>("Int2"), Int3: named<D3>("Int3"), Int4: named<D4>("Int4"),
    Quaternion: named<D4>("Quaternion"),
    Pose: named<PoseT>("Pose"), Pose2D: named<Pose2T>("Pose2D"),
    Transform: named<{ pose: PoseT; parent: string }>("Transform"),
    Transform2D: named<{ pose: Pose2T; parent: string }>("Transform2D"),
    Twist: named<{ linear: D3; angular: D3 }>("Twist"), Twist2D: named<{ linear: D2; angular: number }>("Twist2D"),
    Wrench: named<{ force: D3; torque: D3 }>("Wrench"), Wrench2D: named<{ force: D2; torque: number }>("Wrench2D"),
    GeoPoint: named<{ lat: number; lon: number; alt: number }>("GeoPoint"),
    Matrix3x3: named<Float32Array>("Matrix3x3"), Matrix4x4: named<Float32Array>("Matrix4x4"),
    Timestamp: named<bigint>("Timestamp"), Duration: named<bigint>("Duration"), Uuid: named<Uint8Array>("Uuid"),
    AlignedBox: named<{ min: D3; max: D3 }>("AlignedBox"), OrientedBox: named<{ pose: PoseT; size: D3 }>("OrientedBox"),
    Plane: named<{ position: D3; normal: D3 }>("Plane"), Segment: named<Seg>("Segment"),
    Sphere: named<{ center: D3; radius: number }>("Sphere"), Capsule: named<{ axis: Seg; radius: number }>("Capsule"),
    Cylinder: named<{ axis: Seg; radius: number }>("Cylinder"), Cone: named<{ base: D3; tip: D3; radius: number }>("Cone"),
    Polygon: named<{ points: D3[] }>("Polygon"),
    AlignedBox2D: named<{ min: D2; max: D2 }>("AlignedBox2D"), OrientedBox2D: named<{ pose: Pose2T; size: D2 }>("OrientedBox2D"),
    Plane2D: named<{ position: D2; normal: D2 }>("Plane2D"), Segment2D: named<Seg2>("Segment2D"),
    Circle: named<{ center: D2; radius: number }>("Circle"), Capsule2D: named<{ axis: Seg2; radius: number }>("Capsule2D"),
    Polygon2D: named<{ points: D2[] }>("Polygon2D"),
    CameraIntrinsics: named<{ width: number; height: number; fx: number; fy: number; cx: number; cy: number;
                              model: "NoDistortion" | "BrownConrady" | "Fisheye" | "Rational"; coeffs: Float64Array }>("CameraIntrinsics"),
    JointState: named<{ position: Float64Array; velocity: Float64Array; effort: Float64Array }>("JointState"),
    JointNames: named<{ name: string[] }>("JointNames"),
    Color: named<{ r: number; g: number; b: number; a: number }>("Color"),
    Uri: named<string>("Uri"), Empty: named<Record<string, never>>("Empty"),
    Image: named<ImageValue>("Image"), VideoFrame: named<VideoFrameValue>("VideoFrame"),
    ExternalVideoStream: named<ExternalVideoStreamValue>("ExternalVideoStream"),
} as const;

/* Pairs your member names with the wire's, by position, from the field table the bridge sent. */
export type Rename = { members: [string, string][]; sub: Map<string, Rename> } | null;

export function renamer(shape: Shape | undefined, rows: { path: string; name: string }[]): Rename {
    if (!shape) return null;
    const kids = new Map<string, string[]>();
    for (const r of rows) {
        const cut = r.path.lastIndexOf(".");
        const parent = cut < 0 ? "" : r.path.slice(0, cut);
        if (!kids.has(parent)) kids.set(parent, []);
        kids.get(parent)!.push(r.name);
    }
    const walk = (sh: Shape, path: string): Rename => {
        while (sh.k === "array") { sh = sh.elem; path = path + "[]"; }
        if (sh.k !== "struct") return null;
        const wire = kids.get(path) ?? [];
        const names = Object.keys(sh.fields);
        const out: Rename = { members: names.map((n, i) => [n, wire[i] ?? n]), sub: new Map() };
        names.forEach((n, i) => {
            const w = wire[i] ?? n;
            const r = walk(sh.fields[n], path ? `${path}.${w}` : w);
            if (r) out!.sub.set(n, r);
        });
        const same = out.members.every(([a, b]) => a === b) && out.sub.size === 0;
        return same ? null : out;
    };
    return walk(shape, "");
}

/* Your names to the wire's (toWire) or back. Values not shaped as the schema pass through. */
export function rename(v: unknown, r: Rename, toWire: boolean): unknown {
    if (!r || v === null || typeof v !== "object" || ArrayBuffer.isView(v)) return v;
    if (Array.isArray(v)) return v.map((e) => rename(e, r, toWire));
    const src = v as Record<string, unknown>, out: Record<string, unknown> = {};
    for (const [mine, wire] of r.members) {
        const from = toWire ? mine : wire, to = toWire ? wire : mine;
        if (from in src) out[to] = rename(src[from], r.sub.get(mine) ?? null, toWire);
    }
    return out;
}
