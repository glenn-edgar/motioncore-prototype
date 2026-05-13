# dongle_v1 — single-dongle libcomm base + first app

Fresh build scaffold started 2026-05-11. Captures the v1 dongle infrastructure (Linux base + SAMD21 firmware + first app).

## Scope of v1

**One Linux robot binds to one dongle.** If two physical dongles need to be supported, run two Linux robot processes — one each. Multi-dongle multiplexing within a single Linux process is deferred to v2.

**One app at a time per dongle, but hot-pluggable.** Apps are ChainTree KB trees that subscribe to the base layer's services. Apps can be loaded and unloaded at runtime via the ChainTree runtime without restarting the Linux process.

**The first app is the SAMD21 shell.** System shell (universal commands) + application shell (SAMD21 HIL-specific: gpio / adc / pwm / dac / quad / quad-gen / counter). Implemented as one ChainTree KB tree on Linux paired with one SAMD21 firmware build.

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│  L3 — app (one at a time, hot-pluggable)                 │
│       ChainTree KB tree, matched by manifest tokens      │
│       First app: shell (system + app-shell for SAMD21)   │
├──────────────────────────────────────────────────────────┤
│  L2 — BASE LAYER (single-dongle in v1)                   │
│       ChainTree KB tree:                                 │
│         • discover         (udev / poll /dev/ttyACM*)    │
│         • identify         (libcomm probe)               │
│         • commission       (one-shot per dongle)         │
│         • 3-message sync   (JOIN_REQ → ACK → CONFIRM)    │
│         • link monitor     (low rate, miss-count)        │
│         • manifest cache                                 │
│         • event-subscription routing                     │
├──────────────────────────────────────────────────────────┤
│  L1 — ChainTree runtime + LuaJIT FFI (already exists)    │
├──────────────────────────────────────────────────────────┤
│  L0 — libcomm.so (already exists, slice 1d shipped)      │
└──────────────────────────────────────────────────────────┘
```

## Directory layout

```
dongle_v1/
├── README.md                  ← this file
├── linux/                     Linux side
│   ├── README.md
│   ├── base/                  L2 base layer — ChainTree KB tree
│   │   └── README.md
│   ├── apps/                  L3 application KB trees (hot-pluggable)
│   │   └── shell/             first app: system shell + SAMD21 app shell
│   │       └── README.md
│   └── tests/                 pty-mock-based fixtures (adapted from existing test_comm_pty_multi_dongle.lua pattern)
└── samd21/                    SAMD21 Xiao firmware
    └── README.md
```

## What's reused from existing trees

- **libcomm/** (C library, slice 1d) at `~/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/ros_planner_ii_mqtt_robot/libcomm/` — already has frame (SLIP+CRC8/AUTOSAR), bus_msg_t (40B), comm.h API, router, link FSM skeleton, manifest, ext_bus contract, three transports
- **chain_tree_luajit/** — runtime + DSL frontend
- **comm_ffi.lua** — hand-written FFI binding to `comm.h`
- **pty-mock harness** — `test_comm_pty_multi_dongle.lua` (adapt to single-dongle scenario)

Custody model for the libcomm reuse (copy-in / git submodule / path-reference via `LIBCOMM_PATH`) is **pending decision** before any code is written here.

## What is new in v1

- **L2 base layer KB tree** — extracted from the existing mqtt_robot dongle-lifecycle code, generalized for single-app/single-dongle use
- **L3 shell app KB tree** — driven by SAMD21's manifest; exposes shell commands as Lua-callable wrappers
- **SAMD21 firmware** — new `bus_kernel_samd21.c` (cooperative bare-metal) + new `ext_bus_samd21_usbcdc.c` (TinyUSB) + system shell + app shell
- **libcomm extensions** — additive: `OP_SHELL_EXEC/REPLY`, `OP_ACK`, `OP_EVENT_*`, `OP_GET_MANIFEST` and the corresponding manifest CBOR schema extension

## v1 hard rule — second-dongle behavior

If a second physical dongle plugs in while the Linux base is already bound to a dongle: **reject + warn**. Base logs the device path it saw and the rejection reason. Operator must unplug the duplicate or start a second Linux process. Silent first-come-first-served and disruptive replace are both explicitly rejected.

## Pointers

- Strategic context: `~/.claude/projects/-home-gedgar-motioncore-prototype/memory/four_chip_dongle_pivot_2026-05-11.md`
- Protocol detail: `~/.claude/projects/-home-gedgar-motioncore-prototype/memory/dongle_linux_protocol_2026-05-11.md`
- Project continue: `../continue.md`
- Existing libcomm tree: `~/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/ros_planner_ii_mqtt_robot/`

## What's next

1. **Audit** existing libcomm + `mqtt_robot_main.lua` + `dongle_hal.lua` lifecycle code to identify what's lift-and-shift vs needs rewriting for the L2 base.
2. **Custody decision** for libcomm reuse — copy / submodule / path-reference.
3. **L2 base KB tree** — write the dongle-lifecycle ChainTree program.
4. **L3 shell app KB tree** — write against the existing pty-mock test harness first; real SAMD21 second.
5. **SAMD21 firmware** — bus_kernel_samd21 + ext_bus_samd21_usbcdc + shell core + app shell.
6. **End-to-end validation** — real SAMD21 ↔ Linux base + shell app, against a small command suite.
