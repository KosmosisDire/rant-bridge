/* rant/video: shows and decodes the media in Rant values where a page engine exists (browsers,
 * VS Code webviews, Electron windows). Importing it gives VideoFrame and Image values decode(). */

import { Handle, RantError, Subscriber, Variable, media } from "./node.js";
import type { VideoFrameValue } from "./schema.js";

type Value = Record<string, any>;

/* ---- decoding --------------------------------------------------------------------- */

/* the WebCodecs codec string, H264's read from its SPS so the decoder gets the real profile */
function codecString(codec: string, data: Uint8Array): string {
    if (codec === "H264") {
        for (let i = 0; i + 4 < data.length; i++) {
            if (data[i] === 0 && data[i + 1] === 0 && (data[i + 2] === 1 || (data[i + 2] === 0 && data[i + 3] === 1))) {
                const h = i + (data[i + 2] === 1 ? 3 : 4);
                if (h + 3 < data.length && (data[h] & 0x1f) === 7)
                    return "avc1." + [data[h + 1], data[h + 2], data[h + 3]].map((b) => b.toString(16).padStart(2, "0")).join("");
            }
        }
        return "avc1.42e01e";
    }
    if (codec === "H265") return "hev1.1.6.L120.B0";
    return "av01.0.08M.08";
}

/* Decodes one stream of VideoFrame values, fed every frame in order, since a frame that only
 * holds changes needs the frames before it. Each picture is closed for you when the next one
 * arrives. A subscriber's values decode through one of these by themselves. */
export class FrameDecoder {
    /** @internal */ active = false;
    /* Why frames do not decode, such as a codec this engine lacks. Empty while they do. */
    error = "";
    private decoder: VideoDecoder | null = null;
    private codec = "";
    private waiting: ((f: VideoFrame | null) => void)[] = [];
    private shown: VideoFrame | null = null;

    /* The frame's picture, null until a keyframe opened the stream. */
    decode(v: VideoFrameValue | Value): Promise<VideoFrame | null> {
        this.active = true;
        const data: Uint8Array = v.data;
        if (!data?.length) return Promise.resolve(null);
        if (v.codec === "Mjpeg") return this.image(data);
        if (typeof VideoDecoder === "undefined") return Promise.reject(new RantError("no WebCodecs here: decode needs a page engine and https or localhost"));
        if (!this.decoder && !v.keyframe) return Promise.resolve(null);
        const codec = v.keyframe ? codecString(v.codec, data) : this.codec;
        if (!this.decoder || codec !== this.codec) {
            this.decoder?.close();
            const d: VideoDecoder = new VideoDecoder({
                output: (f) => this.out(f),
                error: (e) => {
                    this.error = `${v.codec} does not decode here: ${e.message}`;
                    if (this.decoder === d) this.decoder = null;
                    for (const w of this.waiting.splice(0)) w(null);
                },
            });
            d.configure({ codec, optimizeForLatency: true, ...(v.width && v.height ? { codedWidth: v.width, codedHeight: v.height } : {}) });
            this.decoder = d;
            this.codec = codec;
        }
        return new Promise((res) => {
            this.waiting.push(res);
            this.decoder!.decode(new EncodedVideoChunk({ type: v.keyframe ? "key" : "delta", timestamp: Number(v.pts ?? 0n), data }));
        });
    }
    private async image(data: Uint8Array): Promise<VideoFrame | null> {
        const bmp = await createImageBitmap(new Blob([data as BlobPart], { type: "image/jpeg" }));
        const f = new VideoFrame(bmp, { timestamp: 0 });
        bmp.close();
        this.out(f);
        return f;
    }
    /* the previous picture is closed for you as the next one arrives */
    private out(f: VideoFrame): void {
        this.error = "";
        this.shown?.close();
        this.shown = f;
        this.waiting.shift()?.(f);
    }
    close(): void { this.decoder?.close(); this.shown?.close(); this.shown = null; }
}

const streams = new WeakMap<Handle, Map<string, FrameDecoder>>();
const listeners = new WeakMap<Handle, Set<(v: Value, field: string[], type: string) => void>>();

const at = (v: any, path: string[]) => path.reduce((o, k) => (o == null ? o : o[k]), v);

/* gives media values their decode(), and feeds active streams even when nobody asks */
media.attach = (value: unknown, handle: Handle, publisher: number) => {
    let byKey = streams.get(handle);
    if (!byKey) { byKey = new Map(); streams.set(handle, byKey); }
    for (const m of handle.value.media) {
        const v = at(value, m.path);
        if (!v || typeof v !== "object") continue;
        if (m.type === "Image") Object.defineProperty(v, "decode", { value: () => decodeImage(v), enumerable: false });
        else {
            const key = `${m.wire}|${publisher}`;
            let s = byKey.get(key);
            if (!s) { s = new FrameDecoder(); byKey.set(key, s); }
            const st = s;
            const feeding = st.active ? st.decode(v) : null;
            Object.defineProperty(v, "decode", { value: () => feeding ?? st.decode(v), enumerable: false });
        }
        for (const l of listeners.get(handle) ?? []) l(v, m.path, m.type);
    }
};

/* JPEG, PNG and raw pixels to a picture, with no state. */
export async function decodeImage(img: Value): Promise<ImageBitmap> {
    const data: Uint8Array = img.data;
    if (img.format === "Jpeg" || img.format === "Png")
        return createImageBitmap(new Blob([data as BlobPart], { type: img.format === "Jpeg" ? "image/jpeg" : "image/png" }));
    const w: number = img.width, h: number = img.height, rgba = new Uint8ClampedArray(w * h * 4);
    const px = img.format === "Rgba8" ? 4 : img.format === "Rgb8" || img.format === "Bgr8" ? 3 : img.format === "Mono8" ? 1 : 0;
    if (!px) throw new RantError(`cannot decode ${img.format} images here`);
    const stride = img.stride || w * px;
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
        const s = y * stride + x * px, o = (y * w + x) * 4;
        if (px === 1) { rgba[o] = rgba[o + 1] = rgba[o + 2] = data[s]; }
        else if (img.format === "Bgr8") { rgba[o] = data[s + 2]; rgba[o + 1] = data[s + 1]; rgba[o + 2] = data[s]; }
        else { rgba[o] = data[s]; rgba[o + 1] = data[s + 1]; rgba[o + 2] = data[s + 2]; }
        rgba[o + 3] = px === 4 ? data[s + 3] : 255;
    }
    return createImageBitmap(new ImageData(rgba, w, h));
}

/* A copy of a decoded picture that stays valid after the stream moves on. */
export function keep(frame: VideoFrame): Promise<ImageBitmap> { return createImageBitmap(frame); }

/* ---- streams ---------------------------------------------------------------------- */

/* A MediaStream for a subscriber's or variable's media: a WebRTC track when the bridge can send
 * one, frames decoded onto a canvas otherwise. */
export function stream(handle: Subscriber<any> | Variable<any>, opts: { field?: string } = {}): MediaStream {
    if (typeof document === "undefined") throw new RantError("video.stream needs a page engine");
    const fields = handle.value.media;
    const field = opts.field === undefined ? fields[0] : fields.find((f) => f.path.join(".") === opts.field);
    if (!field) throw new RantError(`'${handle.name}' has no VideoFrame or Image ${opts.field ? `at '${opts.field}'` : "field"}`);
    const out = new MediaStream();
    const canvas = document.createElement("canvas");
    const ctx = canvas.getContext("2d")!;
    const painted = (canvas as any).captureStream(0) as MediaStream;
    const canvasTrack = painted.getVideoTracks()[0];
    let track: MediaStreamTrack | null = null;
    const show = (t: MediaStreamTrack) => { for (const old of out.getTracks()) out.removeTrack(old); out.addTrack(t); };
    show(canvasTrack);

    const paint = (pic: CanvasImageSource & { displayWidth?: number; width?: number; displayHeight?: number; height?: number }) => {
        const w = pic.displayWidth ?? (pic.width as number), h = pic.displayHeight ?? (pic.height as number);
        if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
        ctx.drawImage(pic, 0, 0);
        (canvasTrack as any).requestFrame?.();
        if (out.getTracks()[0] !== canvasTrack) show(canvasTrack);
    };
    let set = listeners.get(handle);
    if (!set) { set = new Set(); listeners.set(handle, set); }
    set.add(async (v, path) => {
        if (path.join(".") !== field.path.join(".")) return;
        if (v.data && !v.data.length) return;        /* the track carries these pixels */
        try {
            if (field.type === "Image") { const bmp = await decodeImage(v); paint(bmp); bmp.close(); }
            else { const f = await (v as any).decode(); if (f) paint(f); }
        } catch { /* a picture that will not decode is skipped */ }
    });

    /* over WebRTC a VideoFrame field can arrive as a real track, decoded by the browser */
    const node: any = handle.node;
    if (field.type === "VideoFrame" && node.pc) {
        const t: RTCRtpTransceiver = node.pc.addTransceiver("video", { direction: "recvonly" });
        node.videoLines.set(t, { id: handle.id, path: field.wire, onVerdict: (v: string) => {
            if (v !== "ok") { node.videoLines.delete(t); return; }
            track = t.receiver.track;
            track.onunmute = () => show(track!);
        } });
        void node.offer();
    }
    return out;
}

/* The decoder state kept for a handle's streams, released when you are done with it. */
export function release(handle: Handle): void {
    for (const s of streams.get(handle)?.values() ?? []) s.close();
    streams.delete(handle);
    listeners.delete(handle);
}
