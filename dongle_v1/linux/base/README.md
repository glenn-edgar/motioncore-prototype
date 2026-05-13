# L2 base — dongle lifecycle ChainTree KB

Single-dongle base layer for v1. ChainTree KB tree that owns the dongle from discovery to disconnect, exposing services to L3 apps.

## Responsibilities

| Service | Trigger | Output |
|---|---|---|
| **discovery** | `udev` event or periodic poll on `/dev/ttyACM*` | "dongle-found" internal event with device path |
| **identify** | post-discovery | probe via libcomm; returns `(chip_id, dongle_type, firmware_version, commission_state)` |
| **commission** | identify reports uncommissioned | write `(dongle_type, dongle_instance)` to dongle flash; verify by re-identify; one-shot per device |
| **3-message sync handshake** | post-commission, every connect | `JOIN_REQ → JOIN_ACK → JOIN_CONFIRM`; dongle moves to LIVE state; emit `dongle-ready` |
| **manifest fetch** | post-handshake | `OP_GET_MANIFEST` → parse CBOR → cache commands/symbols/events catalogue |
| **link monitor** | timer tick, ~1 Hz | check `comm_node_miss_count` / `comm_node_last_seen_ms`; emit `dongle-gone` if `miss > max_miss` |
| **event subscription routing** | inbound `OP_DONGLE_EVENT` | look up subscribers by `(dongle_id, event_id)`; deliver to registered app handlers; send `OP_ACK` after subscribers return |
| **shell command relay** | L3 app calls `send-shell-cmd` | `OP_SHELL_EXEC` to dongle; aggregate multi-frame `OP_SHELL_REPLY`; return assembled reply |

## State machine (single-dongle v1)

```
        ┌─────────────────────────┐
        │      WAITING            │  ← idle, watching for /dev/ttyACM*
        └────────┬────────────────┘
                 │ device appears
                 ▼
        ┌─────────────────────────┐
        │   IDENTIFYING           │  ← libcomm probe
        └────────┬────────────────┘
        ┌────────┴─────────┐
   uncommissioned       commissioned
        │                    │
        ▼                    │
┌─────────────────┐          │
│  COMMISSIONING  │          │
└────────┬────────┘          │
         │ success           │
         └─────────┬─────────┘
                   ▼
        ┌─────────────────────────┐
        │   HANDSHAKING           │  ← JOIN_REQ → JOIN_ACK → JOIN_CONFIRM
        └────────┬────────────────┘
                 │ success
                 ▼
        ┌─────────────────────────┐
        │   FETCHING_MANIFEST     │  ← OP_GET_MANIFEST
        └────────┬────────────────┘
                 │ success
                 ▼
        ┌─────────────────────────┐
        │   LIVE                  │  ← emit dongle-ready; serve L3 apps
        └────────┬────────────────┘
                 │ link-monitor: miss > max  OR  device removed
                 ▼
        ┌─────────────────────────┐
        │   CLEANUP               │  ← unsubscribe apps; emit dongle-gone
        └────────┬────────────────┘
                 │
                 ▼  (back to WAITING)
```

Any second-dongle device that appears while not in `WAITING` → **rejected with warning** (logged, not bound).

## v1 simplifications

- One state machine, no per-dongle indexing. v2 generalizes by parameterizing on `dongle_idx`.
- One commissioning operation at a time. No queue of pending commissions.
- Link monitor is a single timer, not per-dongle.

## Exposed to L3 (Lua API surface)

```lua
local base = require("base")

base.on_dongle_ready(function(info) ... end)
base.on_dongle_gone(function(info) ... end)

local status, reply = base.send_shell_cmd("tasks")        -- blocks until reply
local sub_id = base.subscribe_event("adc.capture.ready", { pin = 0 })
base.on_event(sub_id, function(ev) ... end)
base.unsubscribe_event(sub_id)

local manifest = base.get_cached_manifest()
```

## Files (not yet written)

```
base/
├── lifecycle.kb            ChainTree KB definition of the state machine above
├── discovery.lua           pyudev-equivalent (luaudev or polling)
├── identify.lua            libcomm probe sequence
├── commission.lua          flash-write commission record via libcomm opcodes
├── handshake.lua           JOIN_REQ/ACK/CONFIRM driver
├── manifest_fetch.lua      OP_GET_MANIFEST + CBOR decode
├── link_monitor.lua        low-rate timer driving comm_node_miss_count
├── event_router.lua        subscription table + OP_DONGLE_EVENT dispatch
├── shell_relay.lua         OP_SHELL_EXEC / OP_SHELL_REPLY aggregator
└── README.md               ← this file
```

## Open questions

- **Does the existing libcomm route `0xFE`-addressed commands without a slave attached?** (Need to read `router.c`.) Leaf dongles are addressed at `0xFE`; if router rejects, this base needs a small libcomm extension.
- **Does `comm_manifest_v1_wire_t` carry capability tokens / commands / events sections?** Or do we need a v2 schema?
- **JOIN_REQ/ACK/CONFIRM is described as "phase-2 work" in link.h — are state transitions for it already implemented somewhere, or do we wire them here?**
