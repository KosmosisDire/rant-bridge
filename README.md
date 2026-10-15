# rant-bridge

Puts Rant on the web. `rant-bridge` runs on a machine on the mesh and gives every
WebSocket client its own Rant node there. `client/` is the TypeScript library that talks to
it, from a browser, Node or a VS Code extension.

## Run it

The usual way is as a service through the CLI:

```
rant bridge enable      # install it as a service for this user and start it
rant bridge status      # running or not, its URL and version
rant bridge logs -f     # follow its log
rant bridge restart     # after editing the config
rant bridge disable
```

Or run it by hand:

```
rant-bridge                     # serve with ~/.rant/bridge.yaml
rant-bridge --init-config       # write that config with a fresh passphrase
rant-bridge --config PATH       # another config
rant-bridge --log-file PATH     # log to a file, rotated at 10 MB
rant-bridge --supervise         # restart it if it ever exits
```

While running it writes `~/.rant/bridge.state` with its pid, URL, version and start time.
`RANT_HOME` moves `~/.rant`.

## Config

`~/.rant/bridge.yaml`, written on first start. Unknown settings are refused.

| Setting | Default | Meaning |
|---|---|---|
| `port` | 7480 | |
| `bind` | `127.0.0.1` | `0.0.0.0` serves the network |
| `passphrase` | random | Every client must send it |
| `allowed_origins` | `[]` | Browser origins allowed to connect, empty allows any |
| `ice` | two public STUN servers | `urls`, and `username` and `credential` for a TURN server |
| `webrtc` | true | |
| `rtc_ports` | any | A range like `50000-50100` for firewalls |
| `mtu` | 1200 | |
| `grace_ms` | 10000 | How long a dropped client may come back to its session |
| `detach_ms` | 1000 | A client silent this long counts as dropped |
| `max_connections` | 32 | |
| `max_message_bytes` | 64 MB | |
| `queue_delay_warn_ms` | 250 | Warn a client whose messages wait this long to be sent |

## Security

- The bridge listens on localhost only by default. Anything on the machine with the
  passphrase can use the mesh.
- To reach it from elsewhere, put it behind a tunnel that authenticates users, such as
  Cloudflare Tunnel with Cloudflare Access, and list your page in `allowed_origins`.
- The passphrase stops strangers, not users of your page: a page that connects has to
  carry it.
- WebRTC traffic is always encrypted. Its signaling goes over the WebSocket.

## The client

```ts
import { RantNode, struct, f64, types } from "rant-client";

const Pose = struct("Pose", { x: f64, y: f64, theta: f64 });

const node = await RantNode.connect("ws://127.0.0.1:7480", { passphrase: "..." });
await node.subscriber("robot/pose", Pose, (pose) => console.log(pose.x, pose.y));
const cmd = await node.publisher("robot/cmd_vel", types.Twist);
cmd.send({ linear: { x: 0.2, y: 0, z: 0 }, angular: { x: 0, y: 0, z: 0.1 } });
```

- Schemas are objects built from `u8` to `f64`, `bool`, `string`, `array`, `enumOf`,
  `struct` and `map`, or the standard types in `types`. `Infer<typeof Pose>` is the value
  type.
- Every Rant pattern is there: `publisher`, `subscriber`, `functionDefinition`,
  `remoteFunction`, `taskDefinition`, `remoteTask`, `variableDefinition`, `remoteVariable`.
- `node.reflection` lists peers and entities. `node.connection` gives the state, the carrier
  and the round trip. `node.onEvent` hears peers come and go, losses and errors.
- The node reconnects by itself. A short drop resumes the same session. A long one makes
  every handle again on a new session. Nothing sent while away is replayed.
- `rant-client/video` decodes video and images in browsers, and turns a subscriber's video
  into a `MediaStream` over WebRTC.

## Build

Needs CMake 3.21, a C++17 compiler, git and Node 22.

```
cmake --preset windows        # or linux, macos
cmake --build --preset windows
cd client && npm install && npm run build && cd ..
ctest --preset windows
```

The exe lands in `bin/`. The `-local` presets build against `../rant` instead of the
pinned Rant release. The client test starts a bridge and needs a working Rant mesh on the
machine.

[PROTOCOL.md](PROTOCOL.md) is the wire protocol, for writing another client.
