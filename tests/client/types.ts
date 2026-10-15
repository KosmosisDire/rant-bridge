/* Compile only: the types a schema gives its values. npm run check in client/ runs it. */

import { array, bool, enumOf, f32, f64, i32, string, struct, types, u64, u8 } from "../../client/src/rant.js";
import type { Infer, Message, RantNode } from "../../client/src/rant.js";

type Equal<A, B> = (<T>() => T extends A ? 1 : 2) extends (<T>() => T extends B ? 1 : 2) ? true : false;
const expect = <T extends true>(_: T) => {};

const Reading = struct("Reading", {
    stampUs: u64, value: f64, label: string(16), on: bool, mode: enumOf({ Idle: 0, Run: 1 }),
    samples: array(f32), raw: array(u8, 4), counts: array(i32), points: array(types.Float3), flags: array(bool),
});
type R = Infer<typeof Reading>;

expect<Equal<R["stampUs"], bigint>>(true);
expect<Equal<R["value"], number>>(true);
expect<Equal<R["label"], string>>(true);
expect<Equal<R["mode"], "Idle" | "Run">>(true);
expect<Equal<R["samples"], Float32Array>>(true);
expect<Equal<R["raw"], Uint8Array>>(true);
expect<Equal<R["counts"], Int32Array>>(true);
expect<Equal<R["points"], { x: number; y: number; z: number }[]>>(true);
expect<Equal<R["flags"], boolean[]>>(true);

export async function handles(node: RantNode): Promise<void> {
    const pub = await node.publisher("reading", Reading);
    pub.send({ stampUs: 1n, value: 2, label: "a", on: true, mode: "Run", samples: new Float32Array(2),
               raw: new Uint8Array(4), counts: new Int32Array(0), points: [], flags: [] });
    // @ts-expect-error a member is missing
    pub.send({ value: 2 });
    // @ts-expect-error not an option of the enum
    pub.send({ stampUs: 1n, value: 2, label: "a", on: true, mode: "Walk", samples: new Float32Array(2),
               raw: new Uint8Array(4), counts: new Int32Array(0), points: [], flags: [] });

    const add = await node.remoteFunction("add", struct("AddReq", { a: i32, b: i32 }), struct("AddRsp", { sum: i32 }));
    const r = await add.call({ a: 1, b: 2 });
    expect<Equal<typeof r.value, { sum: number } | undefined>>(true);

    const pose = await node.remoteVariable("pose", types.Pose2D);
    expect<Equal<ReturnType<typeof pose.get>, { position: { x: number; y: number }; angle: number } | undefined>>(true);

    const raw = await node.subscriber("raw", null);
    const m = await raw.take();
    expect<Equal<typeof m, Message<unknown> | null>>(true);

    const v = (await node.subscriber("reading", Reading)).latest();
    const got = await v;
    // values are plain objects you may change
    if (got) got.value.label = "edited";
}
