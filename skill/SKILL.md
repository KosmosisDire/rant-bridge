---
name: bridge
description: The Rant bridge, which lends this host's UDP sockets to Rant nodes in web pages and on other networks. Use when the user wants Rant in a browser or a web frontend, a node to join a mesh across networks, a NAT or a container, or edits ~/.rant/bridge.yaml.
---

# Rant bridge

The bridge is a background server that lends this host's UDP sockets to Rant nodes elsewhere:
pages, which have none, and programs on another network, so they join this host's mesh. It is
the package `bridge`, and its group `Bridge` runs it like any group, no admin needed.

| goal | command |
|---|---|
| install | `rant install bridge`, `--from PATH` for a bridge you built |
| run it | `rant run Bridge`, `rant stop Bridge`, `rant restart Bridge` |
| at every boot | `rant enable Bridge`, `rant disable Bridge` |
| output | `rant log Bridge`, `-f` to follow |

- Its config is `~/.rant/bridge.yaml`, written at its first start. It listens on every
  interface on 6731 unless the config says otherwise. Edit it by hand, then run
  `rant restart Bridge`.
- Bridges find each other over mDNS: each answers `rant-bridges.local` and lists the others,
  which `findBridges()` from `rant-middleware/tunnel` reads from any page. `advertise: false`
  in the config turns it off.
- Security: the bridge asks no secret, since a program that reaches its port is on the
  network already. A page opens a session only when `allowed_origins` lists its origin:
  `local` (pages on this computer and the LAN) and `https://explore.rantlib.dev` by default,
  `"*"` for any. Another page's connect fails with a message naming the origin to add.
  Anyone who can load an allowed frontend has the bridge's access to the mesh, so restrict
  who reaches it (Cloudflare Access or similar).

## Connecting through it

- A page, a script or an editor extension runs Rant itself through the TypeScript binding,
  the npm package `rant-middleware` each Rant release attaches. It has every pattern and
  reflection, with the same names as the other bindings. In Node.js a node joins the mesh
  directly, and a page passes the bridge's sockets:

```ts
import { openNode, types } from "rant-middleware";
import { TunnelNet } from "rant-middleware/tunnel";
const net = await TunnelNet.connect("ws://192.168.1.20:6731");
const node = await openNode("page", { net });
node.subscriber("Pose", types.Pose2D, (p) => draw(p));
```

- A node in any other binding runs on a bridge's sockets with the bridge option (`net.bridge`
  in C, `bridge` in C++, Python and Rust, `Bridge` in C#, Bridge Url on the Unity component),
  or with `RANT_BRIDGE` in its environment when the code sets none. `ws://` only. An
  unreachable bridge fails the open with the `bridge` error and its reason.
- A machine with no multicast, or behind a NAT (a container, a cloud host), joins a mesh this
  way, through a bridge on that mesh's network.
