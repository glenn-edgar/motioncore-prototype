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

## Resume here (2026-05-23 — slice 2 done)

**Persistence read interface is now complete.** Slice 1
(`latest`+`list_kbs`) AND slice 2 (`stream` with cursor pagination +
size-trim, `latest_stream`, `list_leaves`) are both in. One token-RPC on
`fleet/persistence/query` dispatched by op; service-announce on
`fleet/admin/persistence_service_announce` (30 s periodic + immediate
refresh on new kb). v1 op surface closed. See the
`persistence-query-api-slice2-2026-05-23` memory for the slice-2
specifics (id-based cursor, iterative size-trim, library extensions);
slice-1 memory remains the source for the wire envelope + the two
empirical gotchas (`fleet/admin/persistence_query` poison key, `/tmp` on
WSL2). Acid-tested end-to-end against `farm_soil`'s CIMIS sample stream
(3-page walk at limit=3) + moisture stream (limit=100 trimmed to 6 rows
under the 4 KB cap).

Two empirical gotchas surfaced and got pinned during the slice-1 acid test:
- **Poison key**: `fleet/admin/persistence_query` deterministically
  breaks zenoh-pico's reply routing (server-side `z_query_reply` OK,
  client times out). 5×5 isolated repro; only this exact string. The
  query topic was renamed to `fleet/persistence/query`; the announce
  stays on `fleet/admin/*` for naming parity. Documented in
  `server/persistence/QUERY_API.md` with the zenoh-pico commit and
  exact failing keystr.
- **`/tmp` on WSL2 poisons construct_kb**: defaulting the DB at
  `/tmp/persistence.db` reliably crashes the stream pre-allocation
  with `SQLITE_IOERR_WRITE` (ext 5898) — same ext4 fs as `/home`,
  908 GB free, bare SQLite to `/tmp` works fine, only construct_kb's
  many-fsync pattern fails. Default DB moved to `<repo>/var/persistence.db`
  (run.sh creates the dir; `.gitignore` covers it; main.lua emits a
  WARN if anyone explicitly points the DB at bare `/tmp/foo.db`). See
  `feedback-no-tmp-for-persistent-files` memory for the general rule.

**Earlier today**, the persistence layer landed (layer-30 + decision #6
realized — see `persistence-layer-2026-05-23` memory) and the CIMIS
skill picked up multi-day backfill (drop the 15:00 cutoff, fetch a
trailing 7-day window every retry, publish each newly-finalized day to
both `/sample` stream + `/latest` status leaves — see updated
`cimis-skill-2026-05-22` memory).

### Layout (updated)

```
fleet_design/
  vendor/lua/        ct_* runtime + zenoh bindings + (NEW 2026-05-23)
                     23 kb_sqlite3 Lua files (sqlite3_helpers,
                     knowledge_base_manager, construct_* family,
                     kb_data_structures, kb_status_table, kb_stream,
                     kb_query_support, kb_rpc_*, kb_link_*, bit_mask_*)
  vendor/c/ltree/    (NEW) C source for the SQLite ltree extension —
                     PostgreSQL-style hierarchical path queries.
                     Bench: `sudo make install` -> /usr/local/lib/ltree.so;
                     Container/Pi: run.sh auto-builds.
  robot_common/      shared
    chains/          {connection.lua (KB0 builder),
                      connection_user_functions.lua}
    lib/             {identity, clock (Hinnant date arithmetic +
                      US-Pacific DST rule), zenoh_session,
                      zenoh_rpc_session, app_heartbeat}
    tests/           test_clock.lua (28 deterministic checks)
  fake_robot/        the generic test robot — class_spec, chains/build.lua,
                     the fake_counter app KB
  farm_soil/         the soil-moisture + ET-reference robot
    class_spec.lua   declares moisture + cimis KBs +
                     persistence_topology() + publish_persistence_topology
    chains/build.lua assembles 4 KBs into one IR (67 nodes)
    chains/          connection.json, moisture.lua + user_fns,
                     cimis.lua + user_fns (multi-day backfill)
    lib/             {decoder, ttn_client, moisture,
                      cimis_client, cimis_decoder}
    tests/           {test_decoder, test_moisture, test_ttn_client,
                      test_cimis_decoder, test_cimis_client}
    secrets/ttn.env  TTN_BEARER_TOKEN + CIMIS_APP_KEY (gitignored)
    main.lua         pump loop now republishes persistence_topology
                     every PERSISTENCE_TOPOLOGY_REPUBLISH_S (= 30 s)
  server/fleet_manager/   the controller (register RPC + heartbeat + registry)
  server/persistence/     layer-30. Subscribes to fleet-wide
                          `fleet/admin/persistence_topology_announce`,
                          idempotently construct_kb's per-instance
                          tables, opens per-leaf data subs, dispatches
                          to push_stream_data / set_status_data.
                          NOW ALSO (slice 1, 2026-05-23 late evening)
                          serves a read RPC on `fleet/persistence/query`
                          + announces itself on
                          `fleet/admin/persistence_service_announce`.
                          {main.lua, lib/{persistence,query_server}.lua,
                           QUERY_API.md, test_query_client.{lua,sh},
                           run.sh}.
                          Default DB now `<repo>/var/persistence.db`
                          (NOT /tmp — WSL2 hazard, see memory).
  var/                    (gitignored) bench persistence.db lives here.
```

Each robot's `chains/build.lua` requires the shared KB0 builder + its own
app-KB modules and assembles one `connection.json` IR (farm_soil = 67 nodes
after CIMIS landed; fake_robot = 45).

### farm_soil — the irrigation robot (slices 1–5 + A–D, real-data verified)

A standalone LuaJIT chain_tree robot: polls LoRaWAN soil sensors from The
Things Network AND California's CIMIS Web API and publishes onto the local
Zenoh fabric. Standalone mode (#17) — no DCS, SQLite-only, local zenohd.

- **KB0** (shared) — connect, register, publish namespace, watch the
  controller heartbeat, supervise app KBs.
- **moisture KB** — hourly: fetch the TTN 24 h window → decode → append to a
  256-deep in-memory ring → publish each new reading per-sample on
  `farm_soil/<instance>/<device>/<location>/latest`.
- **`sample` RPC** (`farm_soil/<instance>/sample`) — pull one ring entry by
  index (0 = newest); ad-hoc queries + gap backfill.
- **cimis_station + cimis_spatial KBs** — two KB instances of one CIMIS
  skill module. **As of 2026-05-23**, each runs a 3-gate state machine
  (up-to-date / pre-window / in-window-gap-pending) — the 15:00 cutoff
  was dropped. Each in-window tick fetches a trailing
  `lookback_days = 7` window ending yesterday, then publishes every
  newly-finalized day in date order onto BOTH leaves:
  `…/cimis/<source>/sample` (stream — durable per-day record)
  and `…/cimis/<source>/latest` (status — last-write-wins).
  The 09:00 start remains: before that, CIMIS posts today's row as
  provisional and the filter cannot reliably reject it. 15 min retry
  between attempts; retrying past 15:00 / overnight is fine — the
  robot self-heals gaps end-to-end. The two-KB pattern (one module,
  N instances by source/target — the **skill-KB taxonomy**) is
  reusable for any future multi-source skill.
- **per-KB `repost` RPC** (`farm_soil/<instance>/cimis/<source>/repost`) —
  empty request, returns the latest recorded reading JSON or the literal
  `null`. Solves late-subscriber catch-up (zenoh-pico has no retained
  storage).
- **honest heartbeat** — every ~3 s KB0 publishes `…/heartbeat`
  `{ts, state, apps}`, rolling up each app KB's stamped health.

Verified live:
- moisture: 64–67 real uplinks/fetch from the lacima ranch (3 sensing
  points) — per-sample published + received, the `sample` RPC, heartbeats.
- CIMIS (2026-05-23 10:32 PDT, fresh-state smoke): both KBs ran the
  full 7-day backfill (2026-05-16..05-22) in chronological order onto
  both `/sample` and `/latest` leaves; 28 wire publishes received by an
  independent subscriber in publication order; repost RPC returned the
  freshest day for both sources.
- Persistence (2026-05-23 10:55 PDT): cold persistence.db, robot first,
  persistence subscribed → 74 valid stream rows (7 CIMIS station + 67
  moisture) + 3 status rows. Read-back via KBDS:get_status_data and
  kb.stream:get_latest_stream_data matches the wire byte-for-byte.
- Late-joining persistence (2026-05-23 11:31 PDT): robot up first +
  publishing for 10 s, persistence cold-started; caught the next 30 s
  periodic topology republish, ran schema reconcile (+8 new leaves,
  new kb), opened 8 data subs, began landing heartbeat dispatches.
- Persistence query API slice 1 (2026-05-23 ~14:00 PDT, final smoke):
  test client received the service-announce within seconds, called
  `list_kbs()` → `farm_soil_lacima01 (leaf_count=8)`,
  `latest(kb_name, "heartbeat")` →
  `state=operating apps=[moisture=ok,cimis_station=ok,cimis_spatial=ok]`,
  all three negative envelope checks (unsupported_op / not_found /
  bad_request) returned the right codes. Exit 0.

### THE zenoh lesson — publish per-sample, never blobs

zenoh-pico's pub/sub **silently drops multi-KB payloads** (a ~7 KB value
never arrived; ~600 B is fine — confirmed by a real-data smoke). So the robot
publishes each reading as its own small message, never the 256-ring as one
object. The vendored bindings have **no get-from-storage** and zenohd runs no
storage — which is why the robot holds the ring in memory and serves it by RPC.

### Commits since the 2026-05-21 wrap

`311d440` re-vendor ct_builtins (time-window leaves) · `6c739ee` graduate
KB0 + lib → robot_common · `7bbf981` farm_soil skill modules · `d53ab75`
farm_soil scaffold + moisture KB · `6f6ac8c` republish RPC · `88ebd0b`
per-sample publish + `sample` RPC · `7ae3cd6` slice 5 (KB0 app-KB
heartbeat) · `c4b91b4` wrap (continue.md 2026-05-22) · `7954c52` CIMIS
skill (slices A–D — Pacific clock, cimis_client + decoder, two KB
instances, repost RPC, live-verified). 2026-05-23: `f68d1c1` CIMIS
multi-day backfill (drop 15:00 cutoff, 7-day lookback, sample+latest
leaves) · `59f473c` persistence layer (vendor kb_sqlite3 stack,
server/persistence/, robot-side topology announce + 30s periodic
republish; three-smoke verified). Parallel RA4M1-track commits
(`b9da8b7` mode-2 spectral scaffolding · `ed9965d` wrap · `db4a4eb`
CMSIS-DSP lifted · `83baaed` spectral hardware-verified).
Engine repo (`~/knowledge_base_assembly`): `77ec769b` time-window leaves.
2026-05-23 late evening: `6a22566` persistence query RPC slice 1
(`latest()` + `list_kbs()`, service-announce, envelope + size-cap, two
gotchas pinned with precise workarounds) · `1d2cb3f` slice 2
(`stream()`/`latest_stream()`/`list_leaves()`, id-based cursor +
iterative size-trim; kb_stream gains after_id+order_by; KBDS facade
gap closed).

### Bench smoke

```
# zenohd router:
docker run -d --name fleet-zenohd -p 7447:7447/tcp -p 7447:7447/udp \
  eclipse/zenoh --listen tcp/0.0.0.0:7447 --listen udp/0.0.0.0:7447
# fleet_manager:
unset LUA_CPATH LUA_PATH
server/fleet_manager/run.sh &
# persistence (one-time: cd vendor/c/ltree && sudo make install).
# DB default = <repo>/var/persistence.db — do NOT point at /tmp on
# WSL2 (poisons construct_kb; main.lua emits a WARN if you try).
unset LUA_CPATH LUA_PATH
server/persistence/run.sh &
# farm_soil (needs TTN_BEARER_TOKEN + CIMIS_APP_KEY in farm_soil/secrets/ttn.env):
unset LUA_CPATH LUA_PATH
(cd farm_soil && ROBOT_CLASS=farm_soil ROBOT_INSTANCE=lacima01 \
   IDENTITY_DIR=$PWD/identity ./run.sh) &
# query the persistence DB over Zenoh (acid-test smoke client):
unset LUA_CPATH LUA_PATH
server/persistence/test_query_client.sh
# inspect what persistence is storing (direct SQLite, read-only):
sqlite3 var/persistence.db "SELECT path, label FROM knowledge_base"
sqlite3 var/persistence.db \
  "SELECT path, COUNT(*) FROM knowledge_base_stream WHERE valid=1 GROUP BY path"
# rebuild an IR after a chains/ edit:
luajit farm_soil/chains/build.lua farm_soil/chains/connection.json
```

`unset LUA_CPATH LUA_PATH` is important — the host's interactive shell
has Lua-5.4-ABI paths set by luarocks that crash LuaJIT's cjson loader.
The run.sh scripts set the correct LuaJIT-ABI paths only if the env is
empty.

The vendored bindings are still **client-mode + zenohd-router only** — bench
and container tests need the `zenohd` router above. Pi Zero 2 deploy (no
containers) remains a separate, unsolved problem.

## Plan for next session (after 2026-05-23 — slice-2 wrap)

Persistence query API v1 (slices 1+2) is DONE. The read interface is
complete and proven. Natural next pieces:

**1. Application-gateway sketch (START HERE).** Layers 40+50 from
decision #13. A small HTTP server (LuaJIT + minimal HTTP lib, or
Lua-pico-http, TBD) that **calls the persistence query RPC** (not
direct SQLite) and exposes JSON over HTTP for browsers. Likely surface:

- `GET /robots`                          — list (class, instance) pairs
- `GET /robots/<class>/<inst>/latest`    — every status row for that
                                            instance + the head of each
                                            stream
- `GET /robots/<class>/<inst>/stream/<leaf>?limit=N&since=...`
                                          — stream rows via the
                                            paginated RPC op

Read-only — only persistence writes to the DB; everyone else goes
through `fleet/persistence/query`.

**2. Static dashboard.** Smallest possible front-end on top of the
JSON API. Time-series chart of `cimis.{station,spatial}.sample` ETo
over the last 30 days; a per-device-location panel for moisture (the
TTN uplink stream); a fleet-overview grid showing each robot's last
heartbeat. Defer auth + multi-tenancy.

**3. Open follow-ups inherited from persistence:**
- Explicit decommissioning tool (operator action to retire a removed
  (class, instance) — trim DB rows + kb_info entry).
- zenoh-rs binding for true wildcard subs (deferred until a real
  multi-class operation needs it; see persistence-layer memory).
- zenoh-pico upgrade — re-test the `fleet/admin/persistence_query`
  poison key; if fixed upstream, drop the rename workaround.

**4. Container packaging (still open).** `fake_robot`/`farm_soil` and
`server/{fleet_manager,persistence}` as containers (`FROM
nanodatacenter/luajit-base`, decision #12). Persistence's `run.sh`
already auto-builds ltree.so on first run — the Dockerfile can do
the same at image build. Pi Zero 2 stays bare-process (decision #28).

**First action:** read this file + the
`persistence-query-api-slice2-2026-05-23` (and via it, slice-1 +
layer memories), `farm-soil-robot-2026-05-22`, and `kb-sqlite3-stack`
memories, then sketch the gateway surface in
`server/application_gateway/` (currently nonexistent — pick the HTTP
library first).

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
- Don't publish multi-KB Zenoh values — zenoh-pico silently drops them. Publish per-sample (small messages); serve bulk data by RPC, one small reply per request.
- Don't put SQLite-style persistent files in bare `/tmp` on WSL2 — `construct_kb`'s stream pre-allocation crashes with `SQLITE_IOERR_WRITE` (ext 5898). Use `<repo>/var/` for bench artifacts; containers exempt. See `feedback-no-tmp-for-persistent-files`.
- Don't use `fleet/admin/persistence_query` (or restore the rename without re-testing) — this exact string deterministically breaks zenoh-pico reply routing in commit `88e0ba3`. Other `fleet/admin/*` topics route fine; only this one is poisoned. See `server/persistence/QUERY_API.md` for the repro + provenance.
