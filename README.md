# rant-bridge

A service that lends this host's UDP sockets to Rant nodes elsewhere, pages and programs on
other networks, so they join this host's mesh. Rant's `spec/tunnel.md` is the protocol, and
its `TunnelNet` and native client are the clients.

```
rant bridge enable
```

## Build

```
cmake --preset windows    # or linux, macos
cmake --build --preset windows
```
