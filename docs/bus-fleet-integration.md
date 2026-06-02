# Bus controller — fleet/Zenoh integration: identity, modes, and the operational client API

*Locked 2026-06-02 (design dialog). How the SAMD21 RS-485 bus stack (the C L2
controller from `linux/bus_controller/`, + its SAMD21 bus masters) is wrapped in
LuaJIT and exposed over Zenoh to clients, consistent with the existing fleet_design
conventions. Companion to `bus-controller-two-layer-architecture.md`,
`bus-async-command-lifecycle.md`, `bus-interlock-reporting.md`. Driver: get the API
in shape **before** slave types multiply, so adding a type is config + a handler,
not an API rework.*

## 1. Architecture + the SAMD21 constraint

```
SAMD21 BC  ──USB-CDC──►  C L2 controller   ──FFI──►  LuaJIT wrapper   ──►  Zenoh  ──►  clients
(always; L1 sweep)       (linux/bus_controller,      (a fleet service,        (RPC + pub/sub,
                          the portable byte core)     like persistence)        in OR out of container)
```

- **Bus controllers are ALWAYS SAMD21.** So the L2 controller always runs host-side
  in the container, talking to a SAMD21 over USB; it never lives on the BC chip. The
  C core's M7-portability stays a *property* (it could run elsewhere with non-Zenoh
  command sources) but there is exactly **one deployed topology** to design for.
- **One OS process per logical dongle** (locked packaging): each = C core + LuaJIT
  Zenoh wrapper, single event loop.
- **The C/LuaJIT seam stays byte-oriented:** `submit_command(addr, id, bytes,
  timeout) → bytes`. ALL slave-type knowledge lives above it, in LuaJIT + catalogs.

## 2. Identity — three concepts, not two

We were conflating two things; there are three:

1. **Slot** = `(BC host, bus addr)` — physically unique, exists only to route the sweep.
2. **Chip identity** = `(class, instance)` in the chip — *what the unit is*. **Fungible**:
   many stock chips of a class are interchangeable; the chip carries no slot-specific
   identity ("the firmware maps any slot").
3. **Fleet identity** = the namespace entity `<class>/<instance>` — *what role this
   position fills*. Must be unique fleet-wide.

**The roster binds slot → fleet identity; chip identity is used only to verify/pin.**
Commissioning a chip into a port is the act that **mints a unique slave**, assigning
two things in two scopes: a **bus addr** (unique on that bus — routing) and a **fleet
`(class, instance)`** (unique fleet-wide — namespace). The host owns namespace
allocation; the chip is a verified part.

Roster entry per slot:
```
addr  ⇆  fleet (class, instance)      # the namespace; host-minted, fleet-unique
         expect_class                 # type-check the chip's REGISTER (safety)
         pin = chip_uid (optional)    # lock the slot to ONE unit, or omit (fungible)
```

## 3. Namespace — model B (slave as fleet citizen)

`<class>/<instance>/…`, identical to a robot's namespace. The **bus addr never
appears** — it's internal routing, only required unique on its own bus (the
port-commissioning guarantees that). A slave's identity is **bus-independent**: move
it to another dongle, its addr may change, its namespace does not. The BC host is an
invisible Zenoh **proxy** publishing/serving on each slave's behalf.

## 4. Expected vs found — desired-state reconciliation (fractal)

The host keeps **two sets** and reconciles them continuously:
- **Expected** (from the roster/commissioning) — desired state.
- **Found** (from live REGISTER/liveness on the bus) — observed state. Keyed per bus
  addr: what answered + its reported `class`/`chip_uid`.

| Expected | Found | Match | State | Action |
|---|---|---|---|---|
| ✓ | ✓ | ✓ | **PRESENT** | healthy; namespace live |
| ✓ | ✗ | — | **MISSING** | provisioned slave not answering → maintenance alert |
| ✗ | ✓ | — | **UNEXPECTED** | rogue chip on the bus → wiring/security signal (the zombie worklist) |
| ✓ | ✓ | ✗ | **MISMATCH** | wrong type/unit at slot → **safety stop**; no commands route |

`expect_class`/`pin` are the **match criteria**. The model is **fractal**: the same
reconciliation runs at `system → dongles` (which dongles should exist vs which
registered) and `dongle → slaves`. One engine, two scales. *A dongle is to the
supervisor what a slave is to a dongle.*

## 5. Two modes — offline vs operational

**One API transport, two op-sets, gated by mode.** There is no separate offline
backdoor: offline/admin ops (`verify_topology`, `commission`, `list_found`,
`quarantine`/`adopt`) ride the **same RPC mechanism** as the operational ops — the
dongle's `mode` flag just decides which op-set is live. The dongle owns its USB
exclusively and executes everything; clients (operational *or* offline) never touch
the device.

- **Offline** (maintenance plane): topology verification, commissioning, zombie
  handling. Only available while a dongle is in offline mode.
- **Operational** (runtime plane): the client API below. Topology frozen + verified.

**Why the offline *tools* live in the container** (not a special transport):
**packaging + soft security.** Packaging — the commission/verify CLI tools ship in
the same image as the dongle process + C core + catalogs, so the tooling is always
the version matching the runtime + firmware-catalog it acts on (no tool-vs-runtime
skew, which matters for commissioning + conformance tests). Soft ("somewhat")
security — running them needs container/docker access, a practical barrier leaning on
the **Zenoh-internal-only** trust boundary, *not* a hard gate. Architecturally the
offline tools are **ordinary API clients, just CLI-packaged and in-container**: a tool
talks to the dongle the same way an external app does (the dongle does the USB work on
its behalf). Honest tradeoff: since offline ops share the transport, the docker
boundary isn't a *hard* gate — a Zenoh-internal client could reach a (mode-gated)
offline op; harden with an ACL/admin-token later only if experience demands. Formalizes
the already-locked "commissioning is a standalone tool; operational never commissions."

**The transition is the safety gate:** `offline → operational` succeeds only when the
(multi-dongle) reconciliation is clean — every expected slave PRESENT + verified, zero
MISMATCH, zombies resolved. You never carry commands into an unverified bus.

- **Mode is per logical dongle** (each process owns it — offline one bus, work it,
  re-verify, return it, while other buses keep serving). **"System operational" is the
  AND** — a view, gated by a **thin supervisor (reuse fleet_manager)** that runs the
  dongle-layer reconciliation and opens the system gate.
- **Default posture: everything boots offline; nothing serves the operational API
  until its bus verifies clean and the supervisor opens the gate.** Fail-safe.

## 6. JSON config per logical dongle

Keyed by the **logical dongle id** (commissioned dongle identity), NOT the USB device;
the runtime binds logical→physical at startup by matching the dongle's REGISTER
identity. ttyACM churn never breaks config. Replaces the `.conf` rosters; a
system-level file enumerates all logical dongles.
```json
{ "dongle": { "class": "bus_controller", "instance": 1, "uid": "..." },
  "poll":   { "period_ms": 5, "max_misses": 20 },
  "slaves": [ { "addr": 1, "class": "valve_driver", "instance": 3, "pin": null } ] }
```

## 7. The operational client API

Three interaction kinds → three Zenoh shapes. A client addresses a slave purely by
`<class>/<instance>` — never by dongle or addr. The owning dongle process registers
these channels for *its* operational slaves.

| Kind | Transport | Channel |
|---|---|---|
| **Command** (request→result) | RPC | `<class>/<instance>/cmd` |
| **Observe** (data/health/interlock) | pub/sub | `<class>/<instance>/<leaf>` |
| **Discover** | announce + RPC | `fleet/admin/bus_*_announce`, `fleet/catalog/<class>` |

### 7.1 Command RPC — `<class>/<instance>/cmd`
Synchronous to the client (blocks until result or timeout) even though it's async on
the bus: the wrapper **defers the Zenoh reply** until the C controller's `on_done`
fires (correlated by handle) while its event loop keeps pumping. `timeout_ms` *is* the
exec_timeout. Clients speak **named JSON, never raw ids/bytes.**
```json
// request                                    // reply ok
{ "command":"dac_write","args":{"value":512}, { "ok":true, "result":{}, "elapsed_ms":7 }
  "timeout_ms":1000 }                          // reply err
                                               { "ok":false, "error":{"code":"...","msg":"..."} }
```
Error codes map to stack conditions: `unknown_command`/`bad_args` (catalog validation,
before the wire) · `faulted` (interlock gate) · `busy`/`nak` · `timeout` · `offline`
(dongle not operational / slave MISSING) · `link_down`.

### 7.2 The catalog — `fleet/catalog/<class>` (the type-generalization keystone)
One per **class**, authored with the class_spec, served by the host. The contract that
turns named JSON ⇄ binary bus frames, so **adding a type is "ship a catalog," not an
API change.**
```json
{ "schema":"bus_catalog/1", "class":"valve_driver", "schema_hash":"0x80AEB146",
  "commands": {
    "dac_write": { "id":259, "args":{"value":{"type":"u16"}}, "reply":{} },
    "adc_read":  { "id":260, "args":{"channel":{"type":"u8"}}, "reply":{"value":{"type":"u16"}} } },
  "data": { "flow":{"kind":"stream","type":"f32","unit":"gpm"} } }
```
- **Encoding is host-side** (in the dongle wrapper), catalog-driven (locked, flag 1).
  Type vocabulary mirrors the firmware `shell_reader`/`shell_writer` ABI: `u8/u16/u32/
  i8/i16/i32` (LE), `bool`, `f32`, length-prefixed `bytes`, `enum`, ordered
  `struct/array`. The encoder walks `args` in declared order — exactly the firmware's
  `sr_*` read order; the decoder walks `reply` against the result bytes.
- **Validation before the wire:** `unknown_command`/`bad_args` rejected host-side, so a
  client literally cannot emit a malformed bus frame (robustness + safety).
- `schema_hash` checks catalog vs the slave's reported firmware manifest. Caveat: it
  covers the *command set*, not the *arg layouts* (those are imperative in firmware) —
  so it's a coarse net; back it with a **per-class round-trip conformance test**.

### 7.3 Observation pub/sub — `<class>/<instance>/<leaf>`
- `…/health` — `{ state:present|missing|mismatch, addr, chip_uid, last_seen, schema_ok }`
- `…/interlock` — `{ tripped, detail:{slot,name,tf} }` (the 7a index + 7b pushed message)
- `…/<data-leaf>` — one per `catalog.data` entry.

Declared in the dongle's `persistence_topology` exactly like a robot's leaves → **slave
telemetry is captured by persistence and rendered by the dashboard automatically**, no
new downstream plumbing.

### 7.4 Discovery — two layers
- **Which slaves** → `fleet/admin/bus_roster_announce` (pub): the operational inventory
  (expected/found/state across dongles).
- **How to command a class** → `fleet/catalog/<class>` (RPC/pub), fetched once, cached.
- `fleet/admin/bus_service_announce` mirrors `persistence_service_announce` (service
  handshake: schema version, channel convention, catalog location).

### 7.5 Reach
Named-JSON contract ⇒ the existing `application_gateway` fronts the bus API as HTTP/JSON
(`POST /api/v1/slave/<class>/<instance>/cmd`) for free — non-Zenoh, non-container clients
get the full API.

## 8. Catalog authority (locked, flag 2)
- **Now:** hand-author catalogs per class + a per-class **conformance test** (command
  each entry against a real/sim slave, assert round-trip). Fast/flexible while command
  sets churn; the conformance test (not `schema_hash`) is the real anti-drift net.
- **When a class stabilizes:** introduce **one declarative command spec per class →
  codegen BOTH the firmware marshalling AND the host catalog** from it (drift becomes
  structurally impossible). Fits the project's codegen culture (s_engine, construct_kb,
  nano_data_center class catalog). Hand-authored catalogs seed the specs.
- **Skip runtime self-description** (slave reports its own schema): elegant but costs
  SAMD21 flash/RAM — build-time codegen gives the same drift-freedom at zero MCU cost.

## 9. Adding a slave type (the payoff)
1. Author `fleet/catalog/<newclass>.json` (commands + data schema).
2. Add a `class_spec` (leaves/topology — mostly derivable from `catalog.data`).
3. Commission units into ports (offline, via docker exec).

**No client-API code, no new channels, no dispatcher edits.** Clients become
type-aware by reading the catalog; persistence/dashboard pick up leaves from topology.

## 10. The C↔LuaJIT FFI seam (locked)

Note: the foreign-thread hazard that forces queue+poll for zenoh-pico is **absent**
here — the C controller is single-threaded, driven synchronously by `controller_poll()`
on the LuaJIT main thread (the USB link is polled, not threaded). Queue+poll is still
the right design, for other reasons: LuaJIT FFI callbacks abort JIT traces + are
re-entrancy-constrained; a drain loop lets the wrapper choose *when* it re-enters Lua;
and it makes the deferred-reply mapping clean. It also matches the existing
`zenoh_pubsub`/`zenoh_rpc` idiom.

**Reshape:** the C controller gains a **drainable typed event queue**. The points that
today fire `demux_reply_cb` / `liveness_cb` / `flagged_cb` *also* enqueue a
`ctrl_event_t`. Native C harnesses (bench/capstone) keep their callbacks; the LuaJIT
wrapper registers none and **drains** — non-breaking.

**Two queues, two layers — don't conflate:**
- **Command queue (outbound): per-slave** — depth-5, one-in-flight each (the 6a
  tracker). Per-slave *by necessity* (one-in-flight enforcement + per-slave
  backpressure). Unchanged.
- **Event queue (inbound to LuaJIT): one global FIFO.** Per-slave event queues would
  add N-queue + round-robin draining for no benefit: a global FIFO already preserves
  each slave's event order (its events are a subsequence of the global order), events
  are addr-tagged so the wrapper routes them, and the bus is slow/bounded-rate (no
  flooding slave to isolate). One queue = one clean drain.

```c
typedef struct {                         // FFI-declared, fixed size
  uint8_t  kind;                         // CMD_DONE | LIVENESS | INTERLOCK | PRESENCE | LINK
  uint8_t  addr;  uint8_t status;        // CMD_DONE: shell status / DEMUX_*
  uint32_t handle;                       // CMD_DONE: the submit handle
  uint32_t aux;                          // LIVENESS class_id / INTERLOCK flags / LINK state
  const uint8_t *data; uint16_t data_len;// result/message bytes, valid until next drain
} ctrl_event_t;
int controller_drain(controller_t*, ctrl_event_t* out);  // 1=got one, 0=empty
```

**The wrapper's single event loop** (the locked one-loop decision):
```
loop:
  for req in zenoh_cmd_queue:poll():            -- incoming commands (operational or admin)
      h = controller_submit_command(addr, id, encode(req), timeout); pending[h] = req
  controller_poll(ctrl)                          -- pump the bus
  while controller_drain(ctrl, ev):              -- fan events out
      CMD_DONE  -> req = pending[ev.handle]; req:reply(decode(ev)); pending[ev.handle]=nil
      INTERLOCK/PRESENCE/LIVENESS -> ps:publish(addr → <class>/<instance>/<leaf>, ...)
  periodic: republish health / roster
```
**Deferred reply** = `pending[handle] = held Zenoh request`; the `CMD_DONE` event (the
tracker emits it on reply *or* exec-timeout) resolves it — so the wrapper needs no
bus-side timer. Variable payloads are copied into Lua on drain (valid only until the
next drain — the zenoh_pubsub idiom). Seam stays small: `submit_command(...)→handle` +
`drain()→events` + synchronous getters (`interlock_state`, `slave_state`).

## 11. Container packaging (locked)

- **One LuaJIT process per logical dongle** (C core via FFI), launched by `start.sh`
  **after** the back-office (zenohd → fleet_manager → persistence/gateway/notification),
  in the existing staggered phase — they behave like robots (register + announce onto
  live subs).
- **Logical→physical USB binding by identity, not path:** each process scans
  `/dev/ttyACM*`, reads each REGISTER identity, and **claims (exclusive `flock`) the
  device matching its configured logical-dongle id**, releasing the rest. Survives
  ttyACM churn + multiple dongles without path coupling.
- **fleet_manager = the bus supervisor:** dongle processes register to it with their
  dongle identity + bus roster; it runs the **dongle-layer** expected/found
  reconciliation, owns the **system operational gate**, and publishes the inventory.
  No new process.
- **Offline tools = CLI-packaged API clients in the image** (see §5): they drive the
  running dongle process over the same API; the process (owning the USB) does the work.
  Purpose = packaging + soft security.
- **Deployment plumbing:** ttyACM devices mapped into the container (`--device` /
  device-cgroup; note Docker's static-device-mapping vs hotplug — may want a
  device-cgroup + `/dev` bind on the Pi). Per-dongle JSON configs + the system-topology
  file bind-mounted, like the identity dirs.
- **Boot:** every dongle boots **offline** → self-verifies → reports to fleet_manager →
  fleet_manager opens the system gate when all reconcile clean → processes flip
  operational, client API goes live. Fail-safe.

## 12. Onboarding a new slave type: dongle mode → slave mode (locked 2026-06-02)

How a new slave TYPE comes into existence — develop + validate it as a **dongle over
the production API**, then deploy it as a **slave**. Enabled by the catalog/API being
**transport-agnostic**: the same firmware command table → the same catalog → the same
`<class>/<instance>/cmd` RPC, whether the chip is a dongle on its own USB (commands →
`addr 0` = the chip itself) or a slave on the bus (routed through the BC to `addr N`).
Only transport + addr change, invisibly to clients.

**Lifecycle:**
1. **Dev in dongle mode** — flash the chip `ROLE=dongle`, plug it into its own USB; run
   the bus_controller image in a **dongle mode** that skips the roster/sweep and routes
   the cmd RPC to `addr 0` via the ungated/direct path, under a dev namespace
   (`<class>/dev/cmd`). Develop the firmware features **and the catalog together**,
   validating with the per-class **conformance test** over the *same* client API.
   (Offline/dev plane — the chip isn't on a bus.)
2. **Move to slave mode** — re-flash `ROLE=slave`, re-wire (own USB → RS-485 bus behind a
   BC), and **commission** (offline, via `docker exec` into the bus container that owns
   the BC): assign bus addr + fleet `(class, instance)`, recorded in the roster. The bus
   container's expected/found flips it to PRESENT and serves `<class>/<instance>/cmd`.

| Stays identical | Changes |
|---|---|
| client API (`…/cmd`), the **catalog**, the conformance test | role flash (dongle→slave), wiring (USB→RS-485), addr (`0`→`N`), namespace (`/dev`→`/<instance>`) |

**Container mechanics:** flashing is a HOST op (UF2 bootloader), never in the container;
one image, two modes (`dongle` | `bus`, role auto-detectable from the chip's REGISTER);
commissioning stays offline via `docker exec`, reusing the bus's own USB + C controller.

**To add (small):** a **dongle mode in the service** — connect, *skip provisioning* (a
dongle would PROV_FAIL the bus_controller ladder), route the cmd RPC to `addr 0`
ungated, publish the in-development catalog under the dev namespace.

**SEQUENCING: this lands BEFORE real RS-485 slaves** — you can't sensibly bring a new
slave type onto a bus without first developing + validating its firmware/catalog as a
dongle. Dongle mode + the catalog/conformance tooling = the slave-type onboarding kit;
it gates real-slave deployment.

## 13. Deferred / "experience will shape it"
- **Offline-tool CLI surface** (exact commission/zombie verbs) — kept deliberately thin
  until real use.
- **Hard access control** on admin ops (ACL/admin-token) — only if the soft
  container/trust boundary proves insufficient.
- **Declarative command-spec → codegen** (firmware marshalling + catalog) — once a
  class's command set stabilizes (§8).
