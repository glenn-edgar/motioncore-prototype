# Bus controller — two-layer architecture

*Locked 2026-05-31 (design dialog). Governs the whole RS-485 / multi-drop bus
stack and its portability across device tiers. Companion to
`docs/rs485-bus-protocol-bc2-bc3.md` (wire format) and
`docs/rs485-slave-protocol-*` / the bus-management memories.*

## 1. The problem this solves

A bus controller (the master that polls slaves, tracks liveness, relays
commands/events between clients and the bus) must run on very different hosts
over time:

- a **Pi process** driving a slow SAMD21 RS-485 bus over USB-CDC,
- **on-chip** on a heavy M33/M7 dongle (Arduino Giga class),
- on a **thread / Zenoh interface** where the bus controller sits somewhere on
  the logical thread interconnect.

The seam latency between "the thing scheduling polls" and "the bus peripheral"
ranges from a function call (same chip) to USB to a network hop. A single design
must absorb that range without rewriting the controller, and without ever
letting slow seam latency throttle the bus.

## 2. The two layers

### Layer 1 — wire / sweep layer
**Always in the ISR, co-located with the bus peripheral, never crosses the seam.**
Hard-real-time, bus-rate, local.

Owns:
- frame assembly / CRC / 9-bit MPCM addressing,
- poll **window** timing,
- the **fast empty-poll sweep itself**:
  - *master side:* `POLL → (NO_MESSAGE/summary terminator | timeout) → next POLL`,
    advanced in the ISR chain so the bus stays saturated during the routine
    "everyone quiet" sweep,
  - *slave side:* on a POLL addressed to me, the RX-complete ISR shifts out a
    **pre-staged** window (summary-bit + any queued outbound + terminator) with
    zero main-loop compute,
- the **who-to-poll roster + sweep state machine** — because they must run at
  window speed.

Small, re-implemented per peripheral, never moves.

### Layer 2 — content / controller layer
**Portable C. Relocatable across tiers.** Soft-real-time, paced/throttled.

Owns:
- content handling: inject a client command, fetch detail from a slave that set
  its summary-bit, decide liveness DOWN/UP, run retry timers,
- config: roster contents, poll rate, throttle,
- demux (reply vs. async, by opcode),
- the **command-source interface / network link** up to clients.

Identical C across all tiers; only the transport HAL and client binding change.

## 3. The invariant (every tier)

> **The fast empty-poll sweep always runs in the ISR, below the seam. Content
> escalates up to Layer 2 only when a frame carries content.**

Rationale: window timing is hard-real-time and must run at bus rate; the seam
latency varies wildly per tier; therefore the sweep can never cross the seam — it
lives in silicon, always. Content is paced anyway, so it tolerates the seam.

**Corollary:** the Pi (Layer 2) paces **content**; the ISR (Layer 1) paces
**windows**. This reconciles "the Pi throttles the bus to a slow rate" with
"maximize poll windows to minimise slave↔slave latency" — they are different
cadences on different layers. The empty sweep runs flat-out so windows are
plentiful; Layer 2 injects content into that fast sweep when it has something to
say.

## 4. Per-tier mapping

| Tier | Layer 1 (ISR sweep) | Layer 2 (C controller) | Seam |
|------|---------------------|------------------------|------|
| SAMD21 slow bus | SAMD21 SERCOM ISR | Pi C process | USB-CDC (slow, content-only) |
| M33/M7 heavy dongle | M33/M7 UART ISR | same chip | function call / shared mem (fast) |
| Thread / Zenoh device | host of the bus peripheral | thread / Zenoh node | IPC / network (content-only) |

The Pi's role shifts down the tiers: **host of the controller** (SAMD21) → **one
command source among many local processes** (M33/M7) → **remote command source
over Zenoh** (thread device). The bus controller is a **role, not a location** —
the same insight as the four-chip dongle pivot, applied to the controller brain.

**The host is not necessarily a Pi.** Both the Arduino Giga and the Teensy expose
a **host USB 2 port**, so either can enumerate a downstream SAMD21 dongle over
USB-CDC and run the controller itself. The "Pi C process" row above is really
"any host-USB-capable device running the procedure shell" — Pi, Giga-as-host, or
Teensy-as-host all play the same part. Tier 1 (slow SAMD21 bus, hosted over USB)
and tier 2 (heavy M33/M7 dongle) therefore overlap: a heavy dongle can *be* the
USB host of a SAMD21 bus rather than only being a bus peripheral itself.

### Layer 2 = a C routine + a per-platform "procedure shell"

Layer 2 splits cleanly into a portable **core C routine** (roster · scheduler ·
demux · retry · command-source interface) and a thin **procedure shell** that
wraps it for the platform. The shell does the platform-specific lifecycle: open
the link endpoint, bind the command-source transport, load config, run the event
loop. The *routine* never changes across tiers; only the *shell* does.

- **On Linux: one procedure (OS process) per dongle, configurable by dongle id.**
  Each process owns exactly one device, runs one controller routine, and is
  parameterized by its dongle id (which device, which roster, which Zenoh
  namespace). Add a dongle → launch another procedure. (This supersedes the
  earlier "one process hosting N controllers" packaging: one process *per*
  dongle, not N controllers in one process — cleaner isolation, independent
  restart, and the dongle id is the single config key.)
- **On a heavy dongle (Giga/Teensy): the shell is the firmware task** hosting the
  same routine; the link endpoint is a function call instead of USB.
- **On a thread/Zenoh device: the shell is the node** hosting the routine; the
  link endpoint is the network shim.

## 5. The portability keystone — the escalation queue

The contract by which the ISR sweep hands content up to Layer 2:

```
  Layer 1 (ISR sweep)  ──escalation queue──►  Layer 2 (C controller)
     content frames + state-change events
```

This is the new portability seam, one layer below the libcomm / `comm_transport`
keystone. It **must be a uniform contract across all tiers** so the same Layer-2
C consumes it identically whether the producer is a SAMD21 ISR over USB or an M33
ISR in the next function call. Get this contract right once and Layer 2 is
genuinely tier-agnostic.

## 6. Command sources are plural

The **console / API is a Pi service** — a *command source / client*, not the bus
controller. Multiple command sources submit commands and consume events through
Layer 2's command-source interface: the Pi service/console, other Pi processes,
M33/M7 local threads, Zenoh nodes.

Over the single USB-CDC pipe (one RX decoder + one TX ring — never two handlers)
two **logical streams** are demuxed **by opcode**:

| Stream | Opcodes | Correlation |
|---|---|---|
| command/reply (sync) | `OP_SHELL_EXEC` → `OP_SHELL_REPLY` | `request_id` |
| data/event (async) | `OP_EVENT`, `OP_DBG_LOG`, `OP_HEARTBEAT`, `OP_BUS_SLAVE_DOWN/UP`, forwarded slave data | none |

Pi-side handler requirements: **monotonic request_id** allocation (never reuse),
opcode dispatch (reply → resolve pending; async → event router; unknown/stale →
drop+log), and **flush the device TX ring on host (re)attach** so stale frames
can't mask live ones. (DTR assert makes the device log "host reattach → reset to
BOOT", so a multi-step test must run in **one** console session.)

## 7. Async + interlock model

The bus is a **slow deterministic control plane, not a data plane** — SMBus on a
VME backplane: health, config, presence, coordination; determinism and
observability over throughput. Equivalently, a **distributed lock-step AVR**:
each slave is an autonomous hard-real-time safety node enforcing its own
interlocks at ~1 kHz (instant local guarantee); the bus is the coordination
fabric, **not** in the safety loop.

Locked rules:

1. **Summary-bit in every poll response.** The terminator carries a 1-bit "I
   have an interlock active / something to report" flag, learned on the normal
   round-robin. The controller requests detail only from a flagged slave.
2. **Local-acts-locally (the safety rule).** A trip kills its own output
   on-chip immediately; the async/Pi notification is **advisory** (coordinate
   peers, log, alert). Nothing safety-critical ever waits on a bus round-trip.
   Async event = best-effort fast path; polled latched status = reliable backstop.
3. **Pi-only retries.** Slaves never retry. Pi→slave: no reply within timeout →
   Pi re-sends the whole command. slave→Pi: fire-and-forget UDP; the BC drops a
   bad-CRC frame; the Pi re-asks.
4. **slave↔slave async = UDP, slave-handled.** e.g. a steering slave tells two
   motor slaves "system breaking down"; the receiving slave owns the
   consequence. These move only during poll windows, so the fast in-ISR empty
   sweep is what bounds slave↔slave latency. (Canonical example: three
   I²C-coupled RA4M1 modules — two motor, one steering.)
5. **Scheduler throttles content** to a bounded modest rate and **guarantees
   every roster slave is visited within a bounded interval** (summary-bit never
   stale beyond one cycle; no chatty slave starves the interlock sweep). This
   bounded round-robin is where system-level real-time lives.

## 8. Impact on Stage 3 (consequence — not yet executed)

The poll engine built **on the SAMD21** (Stage 3a, currently uncommitted) splits:

- **Stays on SAMD21 (Layer 1, in/near ISR):** empty-poll sweep, who-to-poll
  roster, summary-bit terminator, respond-when-polled, local interlock reflex.
- **Moves to Pi-side Layer 2 C:** liveness DOWN/UP policy, retry, command-source
  interface, content scheduling/throttle.

Net: the SAMD21 is a **fast Layer-1 bridge** that runs the sweep locally and
escalates to the Pi controller **only on content** (summary-bit set, state
change, queued command). It is *not* a fully dumb bridge (USB latency forbids
Pi-paced windowing) and *not* the full brain. The Stage-3a on-chip DOWN/UP/retry
code is parked pending this migration.

## 9. Why C for Layer 2

LuaJIT cannot run bare-metal on M33/M7; C can. Layer 2 in C + a transport HAL
(`read` / `write` / `encode` / `decode`) + a command-source interface compiles
for the Pi process, the M33/M7 dongle, and a Zenoh node alike. Only the HAL and
the client binding change.
