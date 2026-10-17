# rant-bridge

A service that lends this host's UDP sockets to Rant nodes running in web pages, so they join
the mesh. Rant's `spec/tunnel.md` is the protocol, and its `TunnelNet` is the client.

```
rant bridge enable
```

## Build

```
cmake --preset windows    # or linux, macos
cmake --build --preset windows
```
