# fleet_design — Continue From Here

## Status (2026-05-19)

**Design phase complete + connection KB fully implemented end-to-end.**

All five states of the connection state machine are real (no stubs):
`connecting` → `ack'd` → `namespace_up` → `operating` → on disconnect → `disconnected` → back to `connecting`. Smoke-tested through the happy path, kill-mid-run disconnect detection, and recovery on bench_manager restart.

Two-clock model in place: `CLOCK_MONOTONIC` ms for elapsed-time math (timeouts, backoff, heartbeat cadence) and `CLOCK_REALTIME` for wire `ts` fields + wall-clock boundary events. main.lua's pump emits `CFL_{SECOND,MINUTE,HOUR,DAY,MONTH,YEAR}_EVENT` to the chain_tree on boundary crossings, with a clock-jump guard for Pi Zero 2 NTP-step cascades. Verified: a 90 s run captured `CFL_MINUTE_EVENT @ 2026-05-19 20:16:00 UTC` at the exact minute boundary.

History: design dialog opened 2026-05-18 (26 base decisions); extended 2026-05-19 with amendments #27–#33 — see `memory.md` top section.

## What this directory is

- `memory.md` — load-bearing locked decisions + design rationale + push-backs captured. **Read this before any implementation work in this area.**
- `continue.md` — this file. Current state + open questions + next-session candidates.
- `server/` — Path 1 robot controller. `fleet_manager/` layer built 2026-05-21 (register RPC + heartbeat + in-memory registry); see `server/README.md`.
- `fake_robot/` — Path 2 robot (populated 2026-05-19): `main.lua`, `class_spec.lua`, `chains/` (DSL + user fns + compiled IR), `lib/` (identity + pubsub + RPC wrappers)
- `bench_manager/` — retired 2026-05-21; the real controller is `server/fleet_manager/`.

## What this design is

The architectural design for the Linux-side fleet management layer in motioncore. Sits ABOVE the libcomm dongle work; separate from s_engine.

| Layer | What it does |
|---|---|
| Robot controller (encapsulated system) | Hosts internal Zenoh fabric + publish apps |
| Internal namespace | `<class>/<instance>/<leaf>` — robot-sovereign |
| Publish apps | Translate internal Zenoh ↔ external APIs (HTTP / NATS / MQTT / KB) |
| External world | Speaks only to publish apps; never sees Zenoh |

## What this design is NOT

- Not a libcomm replacement — libcomm continues for dongle ↔ container wire
- Not a re-architecture of s_engine — separate substrate
- Not the implementation — pure design as of this date

## Architecture at a glance

```
External world (operator dashboards, fleet ops, analytics, KB consumers)
        │
        ├── HTTP / NATS / MQTT / KB feeds
        │
        ▼
┌─────────────────────────────────────────────────────────────┐
│ Robot Controller                                            │
│                                                             │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  Publish apps (boundary / API gateway)                │  │
│  └────────────────────┬──────────────────────────────────┘  │
│                       │                                     │
│                       │  internal pub/sub (Zenoh)           │
│                       ▼                                     │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  Robot namespaces                                     │  │
│  │    car_window_controller/right_door/state             │  │
│  │    car_window_controller/right_door/position          │  │
│  │    car_window_controller/right_door/desired_state     │  │
│  │    car_window_controller/left_door/...                │  │
│  │    scservo_arm/station_3/...                          │  │
│  │                                                       │  │
│  │  Wildcards for cross-cutting access:                  │  │
│  │    car_window_controller/*/state                      │  │
│  │    **/heartbeat                                       │  │
│  │    **/position                                        │  │
│  └───────────────────────────────────────────────────────┘  │
│                                                             │
│  Linux containers + MCU robots (zenoh-pico over WiFi)       │
│  all participate as peers in the internal fabric            │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

## Locked design decisions (summary — see memory.md for full rationale)

**Namespace / identity / fabric** (locked earlier in 2026-05-18 dialog):

1. Zenoh lives **only inside** the robot controller. External world uses other protocols.
2. Robot identity: `<class_id>/<instance_id>`. Class is firmware-defined; instance is operator-assigned at commissioning.
3. Robot owns its namespace. All robot-related keys live flat under `<class>/<instance>/`.
4. No `telemetry/`, `status/`, `commands/` sub-categories. Discovery via wildcards on leaf names.
5. Three writer roles share the robot subtree: robot, Fleet Manager, publish apps. Authority split by leaf, not subtree.
6. All Zenoh storages are memory-only. Persistence is a separate Persistence Service (SQLite in container), implementation deferred.
7. MCU robots (Pico 2 W) are first-class. zenoh-pico C agent. LittleFS for class+instance storage. Wio Terminal dropped from scope.
8. Hardware UID is metadata, not identity. Hardware swap doesn't change robot identity.
9. No central namespace schema. Firmware IS the schema. Reference model: microservices / OOP encapsulation / OPC UA — NOT ROS or automotive.
10. Management container holds only the `(chip_uid → class, instance, metadata)` registry + Fleet Manager + Persistence Service + publish apps.

**Server container internal architecture** (locked late 2026-05-18 dialog):

11. Server container runs on Pi 4 or Pi 5 only. C code (libcomm + robot agents) is aarch64-portable across Pi Zero 2 / Pi 4/5 / Snapdragon WSL / Pocket Beagle 2 / Arduino Snapdragon — target Cortex-A53 ISA.
12. Server container is built `FROM nanodatacenter/luajit-base:latest`. Reuses its chain-tree supervisor for in-container app management. NOT s6, NOT supervisord.
13. **Five layers, five processes** (Model B — one process per layer): zenohd → fleet_manager → persistence → application_logic → application_gateway. start_order 10/20/30/40/50.
14. Application layer (gateways) and Application logic layer (business) are **separately stacked**, not combined. Both are Zenoh peers on the internal fabric even inside the same container.
15. **Two SQLite DBs**: `registry.db` (fleet_manager, low write) + `persistence.db` (persistence_local, high write, standalone-only).
16. zenohd is NOT platform infrastructure. It ships inside this container; serves only the robots under this controller.
17. **Two operational modes**: standalone (single Pi, manual `docker run`) and managed (under DCS with platform infrastructure).
18. **Mode-aware apps** use option (i): single image carries both variants; per-deployment `app.manifest.json` selects which to bundle. Only `persistence` and `gateway` have mode variants; zenohd/fleet_manager/application_logic stay local in both modes.
19. **DCS integration** uses existing system_node_control three-layer model (construction/runtime/deployment). We don't write orchestration; we declare our container in the master KB.
20. **Robot classes live in `registry.db.classes` runtime table**, NOT in `catalogs/robot_classes.lua`. Two population paths (firmware-announced on first commissioning + optional operator pre-declaration). Adding a new class requires no KB rebuild, no restart.
21. **Planner is an external consumer**, NOT part of application_logic layer. Runs outside our server container; talks to fleet via the gateway layer. Drives the gateway's external API contract (class catalog + instance state + commands). Can be replaced/upgraded independently from the fleet.
22. **Development plan**: three paths (1: server container, 2: fake Linux robot, 3: MCU dongle continuation). Two Claude windows: Window A = `fleet_design/` (Paths 1+2); Window B = `motioncore-prototype/` (Path 3). Standalone-first; managed-mode wiring deferred.
23. **Fake robot is mandatory test scaffolding** with full commissioning model from day one (avoids the mqtt_robot debt). Lives at `fleet_design/fake_robot/`. One binary, N instances via `IDENTITY_DIR` env. Generic behavior (no per-class personalities yet). First commissioned class = generic `fake_robot`. **Class+instance NEVER set via env** — only assigned by commissioning.
24. **All robots share one fleet contract.** Pure Linux robot, Linux-robot-with-dongle, and MCU robot are indistinguishable to the server — same Zenoh `<class>/<instance>` namespace, same commissioning flow, same operational pub/sub. The dongle is **internal implementation**, not a fleet concept. No special "dongle robot" class. No Path 4 for Linux-hosted robot containers — they reuse Path 2's contract.
25. **MCU robot is the next class after Linux pipeline works.** Same fleet contract; different runtime (C on RP2350, zenoh-pico over WiFi, LittleFS for state, chip_uid from BOOTROM). Validates contract holds across runtimes. Deferred until Paths 1+2 are stable.
26. **fake_robot is partly-throwaway, partly-foundation.** Bootstrap is direct LuaJIT. Commissioning state machine + external Zenoh event dispatch + internal event dispatch use **`chain_tree_luajit`** (foundational, reused across all Linux robots). The `chains/` directory graduates out when first real Linux robot lands; `stub_behavior/` is the only true throwaway. **Engine by target: Linux robots = `chain_tree_luajit` (`luajit_programs_and_containers/building_blocks/chain_tree_luajit/`); MCU robots (Pico 2 / ESP32) when they land = `chain_tree_c` (`c_programs_and_containers/build_blocks/chain_tree_c/`).** chain_tree and s_engine are **distinct engines**; s_engine is space-optimized for ARM 32K dongles only (SAMD21, RA4M1) and is OUTSIDE fleet_design scope (Track 3 internal). mqtt_robot is referenced for both pattern and engine choice (it already uses chain_tree_luajit).

**2026-05-19 amendments (decisions #27–#32 — see memory.md top for full text):**

27. **Identity loaded out-of-band, NOT via Zenoh commissioning** — OVERRIDES #23's runtime-Zenoh-commissioning model. Hybrid C scheme: `ROBOT_CLASS` + `ROBOT_INSTANCE` env (required, fail-fast); `IDENTITY_DIR/state.json` for auto-generated chip_uid + first_seen + fw_version. Shared by all Linux robots (`fake_robot/lib/identity.lua`).
28. **Pi Zero 2 is bare-process target** (no Docker; 512 MB RAM unsuitable). Same code runs in container OR via systemd unit / shell launcher; bootstrap reads env from process environment regardless.
29. **Controller merely acknowledges connection** — no validation, no NACK, no uniqueness enforcement. Robot is sovereign; controller is a passive registry. Connection KB has no NACK branch; timeout → retry-with-backoff is the only failure path.
30. **ACK wire shape: Zenoh RPC on `fleet/admin/register`** — synchronous `cli:call(...)`, JSON request `{class, instance, chip_uid, fw_version, capabilities, ts}`, reply `{ok, controller_id, ts, echo_chip_uid}`. Uses existing `zenoh_rpc.lua` binding.
31. **Namespace setup: core leaves + `on_namespace_up` class hook.** Publishers (`state`, `heartbeat`, `capabilities`, `hardware`), subscriber (`desired_state`). Initial publish sequence on entry to `namespace_up`: capabilities → hardware → state=ready → class hook → operating → start 1 Hz heartbeat KB. Each class ships its own `class_spec.lua`. Token-name rule: `fleet/admin/<verb>[_<object>]`.
32. **Disconnect detection via passive controller heartbeat** on `fleet/admin/heartbeat` (1 Hz `{seq, ts}`). 3 s threshold (3 missed heartbeats) → `EV_DISCONNECTED` → close session, reopen, restart from `announced`. Non-blocking; symmetric with robot heartbeat.
33. **Two-clock model: monotonic ms + wall-clock REALTIME.** Both live in `lib/clock.lua`. `now_ms()` (CLOCK_MONOTONIC) for elapsed-time math (timeouts, backoff, heartbeat cadence; blackboard fields end in `_ms`). `wall_now()` (CLOCK_REALTIME + `os.date("!*t")`) for wire `ts` fields and boundary-event emission. main.lua's pump emits `CFL_SECOND/MINUTE/HOUR/DAY/MONTH/YEAR_EVENT` on field changes between consecutive ticks; suppressed on any tick where `|wall_delta - mono_delta| > 1 s` (Pi-Zero-2 NTP-step guard). Class-specific KBs use boundary events for cron-style scheduling; connection KB ignores them.

## Open work after 2026-05-19

**Design phase is complete (decisions #27–#33 all locked).** Connection KB is fully implemented and smoke-tested. Remaining items are downstream:

### Connection KB

- ✓ All five state handlers real (was the next-session work; landed same day)
- ✓ Two-clock model in place; boundary events emitted by pump
- Clock-jump guard untested under a synthetic NTP step (would need `date -s` or VM pause/resume to exercise; logic is straightforward, low risk)

### Bench convenience

- **`run.sh` launcher** for both `fake_robot/` and `bench_manager/` baking in `LUA_CPATH` / `LUA_PATH` / `LD_LIBRARY_PATH` — current bench dev requires typing them every smoke run.
- **`docker compose`** entry for the local `eclipse/zenoh` peer so `make smoke` does the whole loop.

### Class-specific work (next real KB)

- `class_spec.lua` for `fake_robot` still has a stub `on_namespace_up`; first real class spec will declare class-specific pubs/subs (e.g., `fake_counter` publisher) and demonstrate consuming a wall-clock boundary event for a scheduled task.
- First real Linux robot class (likely `car_window_controller` per CWC spec) — when this lands, `fake_robot/lib/` graduates to a shared location (decision #26 was directional, concrete path TBD).

### Server container internals (Path 1 — still open from 2026-05-18)

1. **Internal Zenoh transport between in-container processes** — TCP loopback vs Unix socket.
2. **Disk layout** — bind-mount vs named docker volume for `registry.db`, `persistence.db`.
3. **External port surface** — only the `gateway` app exposes ports; which?
4. **Application gateway split** — one `gateway` process hosting all protocols, or per-protocol?
5. **Where the server container source lives** — `nano_data_center_base/platform_containers/motioncore_server/` vs `motioncore-prototype/server_container/`?
6. **KB schema** for declaring our container in the master KB.
7. **`gateway` placeholder in platform_containers** — separate platform-level container, or stay app-internal?

### Design-broader (still open from earlier in dialog)

8. **Spin-off directory name** — `fleet_design/` is a working name.
9. **First external protocol to build** — likely NATS or KB (driven by planner per decision #21).
10. **SQLite registry schema** — not yet designed.
11. **Operator commissioning UI** — CLI tool, web UI, or both?
12. **MCU bootstrap-locator strategy** — factory-burned URL vs mDNS scout vs BLE pairing.
13. **LittleFS layout** on MCU.
14. **Naming convention discipline** across robot classes.
15. **Auth / ACL** — when blocking (probably when external apps come online).
16. **First real robot class to implement** — likely `car_window_controller` per CWC spec.
17. **Where `chains/` graduates to** when first real Linux robot lands (decision #26 was directional).

### Resolved (no longer open)

- ~~Q1 identity scheme~~ — locked as decision #27 (hybrid C: env + file)
- ~~Q2 ACK wire shape~~ — locked as decision #30 (RPC on `fleet/admin/register`)
- ~~Q3 namespace setup~~ — locked as decision #31 (core leaves + class hook)
- ~~Q4 disconnect detection~~ — locked as decision #32 (passive controller heartbeat)
- ~~Time-source~~ — locked as decision #33 (two-clock model: monotonic ms + wall-clock REALTIME)
- ~~Connection KB state handlers~~ — all five fully implemented and smoke-tested

## Cross-references

| Reference | Where |
|---|---|
| Companion memory file (this design) | `memory.md` (this directory) |
| Internal Zenoh C wrappers (existing) | `~/knowledge_base_assembly/c_programs_and_containers/build_blocks/knowledge_base/zenoh/` |
| LuaJIT Zenoh bindings (existing) | `~/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/knowledge_base/zenoh/` |
| zenoh-pico upstream | `~/src/zenoh-pico` (v1.9.0, commit 88e0ba3) |
| libcomm dongle work (independent track) | `motioncore-prototype/continue.md` (top-level) |
| CWC class spec (relevant robot class) | `motioncore-prototype/car_door_window_controller/continue.md` |
| Auto-memory for motioncore-prototype | `~/.claude/projects/-home-gedgar-motioncore-prototype/memory/` |

## How to resume (end of 2026-05-19 session)

```bash
cd /home/gedgar/motioncore-prototype/fleet_design/
cat memory.md       # 2026-05-19 amendments at top, then 2026-05-18 base
cat continue.md     # this file — current state + open work + resume instructions
```

**Cold-start reading order (required before any work):**

1. `memory.md` — start with the **2026-05-19 Amendments** section at the top (decisions #27–#32); then base decisions #1–#26 below.
2. This file's "Locked design decisions" section (decisions #1–#32 summarized).
3. This file's "Open work after 2026-05-19" section.
4. Smoke-test the existing code (see commands below) before adding more — confirms the bench setup still works.

## Bench smoke-test recipe

End-to-end loop: docker zenohd + bench_manager + fake_robot. The `run.sh`
launchers bake in `LUA_CPATH` / `LUA_PATH` (repo-relative, picking out of
`vendor/lua/`) and the bench-only `LD_LIBRARY_PATH` for native `.so` files.

```bash
# 1. Start zenohd if not already running
docker run -d --rm --name zenoh-smoke \
    -p 17447:7447/tcp -p 17447:7447/udp \
    eclipse/zenoh:latest \
    --listen tcp/0.0.0.0:7447 --listen udp/0.0.0.0:7447

# 2. fleet_manager — the robot controller (terminal A)
ZENOH_LOCATOR=tcp/127.0.0.1:17447 ./server/fleet_manager/run.sh

# 3. fake_robot (terminal B)
ROBOT_CLASS=fake_robot ROBOT_INSTANCE=alpha IDENTITY_DIR=/tmp/fake_alpha \
ZENOH_LOCATOR=tcp/127.0.0.1:17447 ./fake_robot/run.sh

# 4. DSL recompile (if you edit chains/connection.lua) — build-time only,
#    needs the external chain_tree DSL builder (not vendored; never runs
#    on Pi). Path is intentionally external.
DSL_DIR=$HOME/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/chain_tree_luajit/lua_dsl
LUA_PATH="$DSL_DIR/?.lua;;" luajit fake_robot/chains/connection.lua fake_robot/chains/connection.json
```

The `run.sh` launchers point `LD_LIBRARY_PATH` at external bench locations
for the four zenoh `.so` files (libzenoh_pubsub / _rpc / _token + libzenohpico).
These will be replaced by `vendor/lib-aarch64/` once we cross-compile for Pi.
Override `LD_LIBRARY_PATH` in the caller env to bypass the bench default.

## Resume here (2026-05-21)

**`fake_robot` runs the full lifecycle against the real controller.** This
session shipped three plan items; `server/` now holds a working controller.

### Done this session (committed)

| Commit | What |
|---|---|
| `c8fb87d` | `fake_counter` app KB — multi-KB spawn / concurrent / sweep / respawn verified |
| `ab52f9d` | `server/fleet_manager` — real controller; `bench_manager` retired |
| `e1c3f37` | `fleet_manager` survives a `zenohd` bounce (pcall the heartbeat publish) |
| `f1005e5` | KB0 — dropped the inert transport-recovery scope (one scope now) |

### KB0 structure (current — `connection.json`, 44 nodes)

```
outer column
  wait_for_event("ZENOH_CONNECTED")   HALT boot-gate — blocks until transport up
  state_machine "protocol_sm"
    "wait_for_ack"      ANNOUNCE_REGISTRATION → wait REGISTRATION_ACK (retry-backoff)
                        → PUBLISH_NAMESPACE → NAMESPACE_UP_HOOK → SPAWN_APP_KBS
    "verify_controller_heartbeat"  verify(TEST_CONTROLLER_HEARTBEAT) + delay + reset;
                        fail → ERROR_CONTROLLER_LOST → back to wait_for_ack
  asm_halt()            terminal — KB0 runs forever
```

ONE recovery scope: the controller heartbeat. The 2026-05-20 design's second,
broader transport-recovery scope (`verify(TEST_ZENOH_CONNECTION)` guarding the
outer column) was removed — it had no detector and was redundant: a `zenohd`
outage stops the controller heartbeat, so the controller scope already drives
the full re-bringup, and zenoh-pico client mode reconnects on its own.

KB0 spawns class application KBs from `class_spec.app_kbs` via `SPAWN_APP_KBS`
→ `ct_runtime.add_test`; one IR (`connection.json`) carries KB0 + every app
KB. `fake_counter` is the throwaway example app KB.

### server/ — robot controller

`server/fleet_manager/` is layer 20 of the eventual five (see `server/README.md`).
Reproduces the register-RPC + heartbeat contract; adds an in-memory registry
(`lib/registry.lua`, the seam for SQLite `registry.db` per decision #15). The
controller is passive — no validation, no NACK (#29). `bench_manager/` retired.

### Verified live (zenohd router + fleet_manager + fake_robot)

- Multi-KB: KB0 spawns `fake_counter`, runs it concurrently, sweeps it on
  controller loss, respawns on recovery.
- Controller-loss recovery: heartbeat stops → `ERROR_CONTROLLER_LOST` →
  re-handshake retry-backoff → re-register → respawn.
- `zenohd` bounce: `fleet_manager` survives (transport down/up logged),
  `fake_robot` recovers via the controller scope, `RE-REG reg#2` recorded.

### Zenoh transport — IMPORTANT (unchanged)

The vendored bindings (`zenoh_pubsub`, `zenoh_rpc`) are **client-mode +
zenohd-router only** — no working peer / no-router path. Bench + container
tests need a `zenohd` router:
`docker run -d --name fleet-zenohd -p 7447:7447/tcp -p 7447:7447/udp eclipse/zenoh --listen tcp/0.0.0.0:7447 --listen udp/0.0.0.0:7447`
Pi Zero 2 deploy (no containers) is an unsolved, separate problem.

### Run the end-to-end smoke test

(the shell may preset a Lua-5.4 `LUA_CPATH`; override it)

```
# 1. router (docker run … above)
# 2. controller:
LUA_CPATH="/usr/local/lib/lua/5.1/?.so;;" server/fleet_manager/run.sh &
# 3. robot:
ROBOT_CLASS=fake_robot ROBOT_INSTANCE=bench01 \
  LUA_CPATH="/usr/local/lib/lua/5.1/?.so;;" fake_robot/run.sh
```

## Plan for next session

Two prior plan items remain (base architecture, container) plus follow-ups:

**1. Base architecture (START HERE).** Work out how a real robot class
composes — class-specific KBs, `class_spec`, identity, the app layer — beyond
the `fake_counter` throwaway. Decide where `fake_robot/lib/` and `chains/`
graduate to a shared location (decision #26 was directional). Likely first
real class: `car_window_controller` per the CWC spec.

**2. Container.** Package `fake_robot` (and later `server/`) as a container —
Dockerfile + `vendor/` + `lib/`. The Pi Zero 2 runs the bare process; the
container is the non-Pi deployment form. `server/` is built `FROM
nanodatacenter/luajit-base` (decision #12).

**3. fleet_manager follow-ups.** SQLite `registry.db` (decision #15) behind
`lib/registry.lua`; then the next controller layers (`persistence`,
`application_logic`, `application_gateway`). Small hardening: KB0's
`PUBLISH_NAMESPACE` does unguarded `ps:publish` calls — pcall them like
`fake_counter` does (low risk; the transport is up right after an ACK).

**First action:** read this file + the `chain_tree_dsl_runtime_model.md`
memory, then design the base architecture for the first real robot class.

**Reference paths (dev-machine orientation only — NOT used at runtime):**
The runtime uses `fleet_design/vendor/lua/` exclusively (see `vendor/PROVENANCE.md`).

- chain_tree DSL (build-time): `~/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/chain_tree_luajit/lua_dsl/`
- chain_tree runtime (`vendor/lua/ct_*.lua` source): `…/chain_tree_luajit/runtime_dict/`
- chain_tree test corpus: `…/chain_tree_luajit/dsl_tests/incremental_binary/`
- Zenoh bindings (`vendor/lua/zenoh_*.lua` + bench `.so` source): `~/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/knowledge_base/zenoh/`

## What NOT to do

- Don't redesign the namespace structure — locked (#1–#10, #31).
- Don't introduce a central schema file — firmware is the schema (#9, #20).
- Don't expose Zenoh to external apps — that's the encapsulation boundary (#1).
- Don't reach for ROS / VDA 5050 patterns — wrong reference model.
- Don't muddle s_engine and chain_tree — distinct engines (s_engine = ARM dongles; chain_tree = fleet_design).
- Don't re-add Zenoh commissioning round-trips — #23 rescinded by #27; identity is env+file.
- The real controller is `server/fleet_manager/`; `bench_manager/` was retired 2026-05-21.
- Don't propose chain_tree DSL from API primitives — study `dsl_tests/` + the `chain_tree_dsl_runtime_model.md` memo first.
