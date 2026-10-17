# Rant bridge protocol (v12)

`rant-bridge` puts a Rant node on the mesh for each client that connects over a WebSocket.
The client speaks this protocol. `client/` is the reference client, and most users only
need its API. This file is for writing another client or for reading the bridge code.

- **Control plane:** JSON text messages on the WebSocket.
- **Data plane:** binary frames, on the WebSocket or on WebRTC data channels.
- **Values:** CBOR, shaped by the entity's schema.

All integers in frames are little endian. All times in JSON are milliseconds with an `_ms`
suffix, or microseconds with `_us`.

## Connection

1. The client opens `ws://host:7480` (or `wss://` behind a tunnel).
2. With `allowed_origins` set, a browser whose `Origin` is not listed is closed with 4003.
3. The client sends `open`. Nothing else is accepted before it.
4. The bridge replies, then pings at the probe interval. The client answers every PING
   with a PONG carrying the same `seq`.

### `open`

```json
{"op":"open","seq":1,"proto":12,"passphrase":"...","name":"my-page","domain":150}
```

| Key | Meaning |
|---|---|
| `proto` | Must be 12. Any other value is refused and the socket closed with 4001. |
| `passphrase` | Must match `bridge.yaml`. A wrong one is refused after 1 s and closed with 4001. |
| `resume` | The token from an earlier reply, to take that session back. |
| `name` | The node name. Empty picks `web-xxxxxxxx`. |
| `probe_interval_ms` | How often the bridge pings this client. Default 250, minimum 10. |
| Node options | `domain`, `prefix`, `max_topics` (default 64), `fetch_details`, `match_wait_ms`, `disable_shm`, `disable_logs`, `disable_meta`, `disable_error_logs`, `multicast_interface`, `seed_peers`, `unicast_only`, `self_ip`, `advertise_port`, `fragment_size`, `recv_buffer_bytes`, `send_buffer_bytes`, `announce_interval_ms`, `peer_timeout_ms`, `max_peers` (default 256) |

Reply:

```json
{"op":"reply","seq":1,"ok":true,"proto":12,"name":"my-page","webrtc":true,
 "resumed":false,"resume_token":"9f...","grace_ms":10000}
```

### Resume

- A dropped connection leaves its session, node and entities alive for `grace_ms`.
- Reconnecting with `resume` set to the last token takes the session back: `resumed` is true
  and every entity is as it was. Each reply carries a fresh token.
- If the session is gone, the reply has `resumed: false` and a new node. The client must
  create its entities again.
- Everything queued toward a detached client is discarded. Nothing old is replayed after a
  resume. Match state is pushed again for every entity.
- WebRTC does not survive a drop. The client negotiates it again.

### Liveness

- The bridge sends a PING every `probe_interval_ms`. Its CBOR payload is what the bridge
  measures of the link: `{us, jitter_us, samples, missed}`.
- Any message from the client counts as heard. Silence longer than `detach_ms` closes the
  socket with 4008 and the session starts its grace period.
- `missed` counts probe intervals gone by without an answer.

### Close codes

| Code | Why |
|---|---|
| 1008 | `slow consumer`: a reliable lane overflowed its 8 MB bound. |
| 4001 | The open was refused: wrong protocol or passphrase. |
| 4003 | `origin not allowed`. |
| 4008 | `no answer to pings`. |

## Control plane

Every request carries `op` and a client chosen `seq`. Every reply is
`{"op":"reply","seq":N,"ok":true,...}` or `{"op":"reply","seq":N,"ok":false,"error":"..."}`.

### `create`

Makes one entity. The client picks its `id`, 1 to 65534, used by every frame after.

```json
{"op":"create","seq":2,"id":1,"kind":"subscriber","name":"camera/front",
 "schema":"struct Pose { x: f64, y: f64 }","reliable":true}
```

| Kind | Schema keys |
|---|---|
| `publisher`, `subscriber` | `schema` |
| `variable_definition`, `remote_variable` | `schema` |
| `function_definition`, `remote_function` | `req`, `rsp` |
| `task_definition`, `remote_task` | `req`, `prg`, `rsp` |

- Schemas are Rant schema text. An empty schema with `reflect: true` takes the type from the
  mesh. An empty schema without it sends raw bytes.
- `raw: true` keeps a typed entity's values as Rant wire bytes both ways, for a client that
  has Rant itself (the web explorer). The reply says `raw: true` when the bridge honours it.
- Topic QoS: `reliable`, `keep_last`, `catch_up`, `max_message_bytes`, `heartbeat_ms`,
  `repair_delay_ms`, `backpressure_wait_ms`, `shm_max_bytes`, `queue_bytes`, `max_rate_hz`,
  `no_timestamp`.
- Function and task options: `timeout_ms`, `backpressure_wait_ms`, `keep_last`,
  `progress_best_effort`, `progress_keep_last`, `no_cancel`, `exclusive`, `multi`.
- Variable options: `read_only`, `allow_force`, `catch_up`, `keep_last`,
  `backpressure_wait_ms`, `on_write` (also push write events).

The reply carries `id`, `reliable`, `reflected` when asked, and one schema block per schema:
`schema`, or `req`, `rsp` and `prg`. A schema block is null for an untyped entity:

```json
{"name":"Pose","hash":"0123456789abcdef","dsl":"struct Pose {...}",
 "fields":[{"path":"x","name":"x","kind":"f64"}]}
```

A field row has `path` (`boxes[].min` inside struct arrays), `name`, `kind`, and when they
apply `named`, `elem`, `elem_named`, `count`, `cap` and `variants` (`[{name, value}]`).
`kind` is one of `u8 u16 u32 u64 i8 i16 i32 i64 f32 f64 bool arr struct string vstring varr
map enum`. A variable's current value follows the reply as a VAR frame.

### Entity ops

| Op | Keys | Reply |
|---|---|---|
| `close` | `id` | `{}` |
| `refresh` | `id` | The create reply plus `retyped` |
| `drain` | `id`, `timeout_ms` (default 1000) | `{drained}` |
| `cancel` | `call` | `{status}`: `ok`, `no_cancel`, `not_pending` or `error` |

### Node ops

| Op | Keys | Reply |
|---|---|---|
| `settle` | `timeout_ms` (absent waits the default) | `{settled}` |
| `log` | `level` (`error`, `warn`, `info`), `text` | `{}` |
| `log_subscribe` | `levels` (default all three) | `{}`, then `log` pushes |
| `stats` | | `{entities:{id:{queued_bytes, queued_msgs, oldest_age_us, dropped}}, link:{carrier, rtt, missed}}` |

### Reflection

| Op | Keys | Reply |
|---|---|---|
| `peers` | | `{peers:[{id, uuid, name, address, active, epoch, catching_up, fragment_size, last_heard_us, rtt, mesh_rtt, rtt_min_us, missed}]}` |
| `entities` | | This node's entities |
| `peer_entities` | `peer` | One peer's entities |
| `mesh` | | Every entity on the mesh |
| `find` | `kind` (`topic`, `function`, `task`, `variable`), `name` | `{entity}` or null |
| `epoch` | | `{epoch}`, which changes whenever the mesh view does |
| `meta` | `peer`, `sections` | `{valid, status, provider, info}` |

- An RTT is `{us, jitter_us, samples}`. `mesh_rtt` is the bridge to the peer. `rtt` adds the
  client's own hop to the bridge, which is what the client actually sees.
- Entity lists come as `{entities:[...], epoch}`. An entity has `kind`, `name`, `provides`,
  `consumes`, `reliable`, `providers`, `consumers`, `generation`, and when they apply
  `provider`, `from`, `writable`, `forceable`, `cancellable`, `exclusive`, `multi`,
  `incomplete`, `conflict`, and schema blocks `schema`, `rsp`, `prg`.

### Pushes

| Op | Keys |
|---|---|
| `event` | `event`: `peer_up`, `peer_down`, `peer_interest`, `msg_lost`, `send_error`, `rtc`, `error`. Plus `text` and the fields of that event (`peer`, `peer_name`, `topic`, `id`, `first`, `count`, `state`, `error`, `code`). |
| `match` | `id`, `count`, `ready`, sent when either changes |
| `warning` | `kind: "queue_delay"`, `id`, `text`, once each time an entity's oldest queued message waits longer than `queue_delay_warn_ms` |
| `log` | `level`, `node`, `wall_us`, `mono_us`, `recv_us`, `written_us`, `text` |
| `rtc` | `candidate`, `mid`: a local ICE candidate |

A `msg_lost` with `peer: 0` means the bridge dropped best effort messages for this client.

## Tunnel

A client that runs its own Rant node (Rant built to wasm, `rant/bindings/web`) opens with
`"tunnel": true`. The bridge opens no node for it and lends it UDP sockets on its host
instead, so the client's node is a node of that host to the mesh. The bridge never reads
the datagrams. `client/src/tunnel.ts` is the reference.

- The open reply adds `tunnel: {interfaces: [{addr, mask}]}`, the host's IPv4 interfaces,
  dotted. The other node and entity ops answer an error.
- Sockets are numbered by the client, 1 to 65535, at most 16 at once.

| Op | Keys | Reply |
|---|---|---|
| `udp_bind` | `sock`, `port` (0 for any), `if` (dotted, default any), `reuse` | `{port}`, the bound port. A bound `sock` is rebound. |
| `udp_member` | `sock`, `group`, `if`, `join` (default true) | `{}` |
| `udp_option` | `sock`, any of `ttl`, `loop`, `own_groups` | `{}` |
| `udp_close` | `sock` | `{}` |

Datagrams travel batched, both ways: one binary message carries one or more, up to 60000
bytes in all.

```
[u8 9][u8 flags]  then per datagram  [u16 sock][4 ip][u16 port][4 via][u16 len][payload]
```

- `ip` is the destination toward the bridge and the source toward the client, in network
  byte order. `via` is the interface to send a group datagram out of, 0 for the default,
  and 0 toward the client. `flags` is 0.
- A side batches what it sends in one go: the client what one task sends, the bridge what
  one wake of its sockets drained.
- Batches ride data channel 1 (unordered, no retransmits) while it is open, else the
  WebSocket, never both at once, so datagrams keep their order. A batch that finds more
  than 4 MB waiting is dropped, as a full socket drops datagrams.
- The bridge sends only to 239/8, to loopback and to addresses on its host's subnets, so
  it relays nothing beyond the mesh.

## Data plane

One frame is one WebSocket binary message or one data channel message.

```
[u8 op][u8 flags][u16 id][u32 seq][u32 peer][u64 written_us][u64 capture_us]
[u8 text_len][text]
[u16 msg][u32 total][u32 offset]     only when flags bit 7 is set
[payload]
```

| Field | Meaning |
|---|---|
| `op` | 1 DATA, 2 VAR, 3 CALL, 4 RESULT, 5 PROGRESS, 6 CANCEL, 7 PING, 8 PONG |
| `flags` | Per op, below. Bit 7 marks a chunk. |
| `id` | The entity, the create `id`. 0 for PING and PONG. |
| `seq` | A call id, a request id or a ping number. 0 for DATA and VAR. |
| `peer` | Toward the client: the publisher, provider, caller or writer. The client sends 0. |
| `written_us` | Toward the client: the writer's wall clock at send, in UTC microseconds. |
| `capture_us` | When the data was true. A client may set it on DATA. |
| `text` | Up to 255 bytes: a caller name or a result message. |

| Op | Client to bridge | Bridge to client |
|---|---|---|
| 1 DATA | Publish on topic `id` | A delivery on topic `id` |
| 2 VAR | Write variable `id`: `flags` 0 set, 1 force, 2 unforce (no payload) | An update: bit 0 forced, bit 1 a write event |
| 3 CALL | Call remote function or task `id`, `seq` a client call id | A request for definition `id`, `seq` the request id, `text` the caller name |
| 4 RESULT | Answer request `seq`: `flags` status, `text` message | The outcome of call `seq` |
| 5 PROGRESS | One progress value for task request `seq` | Progress on call `seq`. An empty payload means the task is running. |
| 6 CANCEL | Never: use the `cancel` op | Cancel requested on task request `seq` |
| 7 PING | Never | `seq`, CBOR link stats |
| 8 PONG | Answer PING `seq`, empty payload | Never |

Call status: 0 ok, 1 app_error, 2 no_handler, 3 timeout, 4 peer_lost, 5 cancelled,
7 no_provider. A call the bridge cannot even send answers 5 with the reason in `text`, so
every call settles.

### Chunks

- A message over 16 KB goes out in chunks. Each chunk has bit 7 set and the extension:
  `msg` numbers the message per entity, `total` is the whole payload size, `offset` is
  where this chunk starts.
- Chunks of one message arrive in order. A newer `msg` on an entity drops an unfinished
  older one. The bridge refuses messages over `max_message_bytes`.

### Scheduling

- Each entity has its own lane toward the client. Lanes take turns one chunk at a time, so
  a large image never holds a small pose back for more than one chunk.
- Best effort lanes hold 1 MB (or the entity's `queue_bytes`) and drop the oldest message
  when full. Drops are reported as `msg_lost` with `peer: 0`.
- Reliable lanes hold 8 MB. Past that the client cannot keep up and is closed with 1008.
- Writes from the client are not acknowledged. A failure comes back as a `send_error` event.

## Values

A typed entity's payload is one CBOR item. An untyped entity's payload is the raw bytes.

| Rant | CBOR |
|---|---|
| Struct | Map keyed by the field names in the schema, which are snake_case |
| `u8` to `u32`, `i8` to `i32` | Integer |
| `u64`, `i64` | Integer, always the 8 byte form so clients can read it as a bigint |
| `f32`, `f64` | Float. NaN and infinities pass through. |
| `bool` | Boolean |
| `string`, `vstring` | Text |
| `enum` | The option name as text. A value with no name stays a number. |
| `map` | A map of numbers, booleans, text, lists and maps |
| Array of `u8` | Byte string |
| Array of other numbers | RFC 8746 typed array: tag 69 u16, 70 u32, 71 u64, 72 i8, 77 i16, 78 i32, 79 i64, 85 f32, 86 f64 over the little endian bytes |
| Array of `bool`, `string`, struct | Array |

Toward the bridge, any number may be a float when it holds a whole value, which is what
JavaScript sends. Out of range values, wrong types and unknown enum names are refused with a
`send_error` naming the field path.

## WebRTC

WebRTC is optional and off when `webrtc: false` in `bridge.yaml`. It lowers latency on
lossy links and carries video as media tracks.

1. `{"op":"rtc"}` starts a peer connection. The reply has `ice_servers`
   (`[{urls, username, credential}]`).
2. The client sends its offer: `{"op":"rtc","sdp":"...","tracks":{...}}`. The reply has the
   answer `sdp` and a verdict per requested track.
3. Both sides trade candidates: the client with `{"op":"rtc","candidate":"...","mid":"..."}`,
   the bridge with `rtc` pushes.
4. State changes arrive as `event: "rtc"` with `state`.

### Data channels

- Channels are negotiated, so neither side announces them: channel `id` is entity `id`, and
  channel 0 carries PING and PONG. A tunnel session has no entities, and channel 1 carries
  its datagrams.
- A reliable entity gets an ordered reliable channel. A best effort one gets an unordered
  channel with no retransmits.
- Frames are the same as on the WebSocket. Whichever carrier has room takes the frame, so a
  client reads frames from both.

### Video tracks

A `VideoFrame` can sit anywhere in a value: a topic, a variable or inside a larger type.
To get one as a media track the offer adds a recvonly video line and names it in `tracks`:

```json
"tracks":{"0":{"id":3,"path":"frame","keep_data":false}}
```

- `path` is the `VideoFrame` field. Empty means the whole value is a `VideoFrame`.
- The verdict is `ok` or the reason the line was refused.
- H264, H265 and AV1 frames go out as RTP on the browser's payload types. Sending starts at
  a keyframe. The RTP time comes from `capture_us`, else `pts`, else `written_us`, so a page
  can line overlays up with the frame on screen.
- The value still arrives as a frame, with the video `data` emptied unless `keep_data` is
  set. MJPEG and codecs the browser did not offer keep their data in the frame.
- Video is bridge to client only. A client publishes video as a normal value.
