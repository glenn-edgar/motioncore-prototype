# Linux side — base layer + applications

The Linux side runs as a single LuaJIT process built on the existing ChainTree runtime + libcomm. Two distinct layers:

- **`base/`** — L2 base layer. ChainTree KB tree managing the dongle lifecycle. Single-dongle in v1.
- **`apps/`** — L3 applications. ChainTree KB trees that consume base-layer services. Hot-pluggable.

## Process model (v1)

```
single Linux process
  ├── ChainTree runtime
  │   ├── base KB tree         (L2 — running continuously)
  │   └── app KB tree(s)       (L3 — loaded against the bound dongle)
  └── libcomm.so via comm_ffi
        └── ext_bus → /dev/ttyACMx (one dongle)
```

If a second dongle is desired, run a second Linux process — see the v1 hard rule in `../README.md`.

## Layer contracts

### L2 → L3 (events the base emits to apps)

- `dongle-ready (chip_id, manifest, capability_tokens)` — emitted after handshake + manifest fetch complete; signals app may begin issuing commands
- `dongle-gone (chip_id, reason)` — link monitor or ext_bus close declared the link dead; app must stop issuing commands and clean up
- `subscribed-event (event_id, seq, timestamp_us, payload)` — per-app subscription delivery

### L3 → L2 (services apps invoke on the base)

- `send-shell-cmd(command_line)` → returns `(status, reply_bytes)` ; multi-frame replies aggregated transparently
- `subscribe-event(event_id, filter_args)` → returns `sub_id`
- `unsubscribe-event(sub_id)`
- `get-cached-manifest()` → returns the post-handshake manifest

### Entry point (planned)

```
luajit motioncore_robot_main.lua <config.json>
```

Modeled on the existing `mqtt_robot_main.lua` entry point in the `ros_planner_ii_mqtt_robot/` tree.

## Subdirectories

- **`base/`** — see `base/README.md`
- **`apps/shell/`** — see `apps/shell/README.md`
- **`tests/`** — pty-mock-based test fixtures, adapted from `test_comm_pty_multi_dongle.lua`
