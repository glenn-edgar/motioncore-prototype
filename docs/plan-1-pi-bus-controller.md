# Plan 1 — Pi-side bus controller (normal command path + interlocks)

*Locked 2026-05-31. The build sequence that delivers the MVP of the bus: the
normal command/control path (AIs/clients driving slaves, command→reply) **plus**
the interlock async path (summary-bit → detail → advisory notify). Implements the
architecture in `docs/bus-controller-two-layer-architecture.md`.*

## Goal of Plan 1

Two payloads, end to end:
1. **Normal command path** — a client (AI / Pi service / console) issues a
   command to a slave and gets a reply, through the autonomous poll sweep.
2. **Interlock async path** — a slave trips locally, flags its summary-bit, the
   controller fetches detail and notifies clients (advisory only).

Out of scope for Plan 1: slave↔slave async, fragmentation, the fast M33/M7 tier,
Zenoh. Those build on this.

## Head starts (this is NOT from scratch)

- `vendor/libcomm/frame.c` is **already C and shared** with the firmware. The
  Pi-side Layer-2 controller links the *same* frame codec. The wire layer is done.
- Phase A runs against the **current** SAMD21 firmware (main-loop bridge + sync +
  `CMD_BUS_*` still present). All the Pi-C plumbing is proven *before* the
  firmware ISR rework (Phase B). Clean risk isolation / bisection.

## The link-endpoint seam (decided: module, single process)

Layer 2 never knows it's on USB. It talks to an abstract **link endpoint**:
- `send_frame(bytes)` / `recv_frame()→bytes` (or callback)
- emits link-state events: `up` / `down`

Everything USB-specific — enumeration (VID:PID `2886:802f`), open/reopen, DTR,
the reset-to-BOOT behaviour, ttyACM re-enumeration swap, SLIP framing, reconnect
backoff — lives **below** the seam in the **USB link manager**.

```
  ┌── Layer 2 bus controller (portable C): roster·scheduler·demux·retry ──┐
  └────────────────────────── link endpoint ─────────────────────────────┘
  ┌── USB link manager (Pi-tier only): enumerate·open·DTR·reset·reconnect·SLIP ─┐
  └────────────────────────────────────────────────────────────────────────────┘
```

The link-endpoint interface is the **same on every tier**; only the
implementation below it changes (USB on Pi; function-call/shared-mem on M33/M7;
network shim on Zenoh). Separating the USB link manager *is* defining that seam.

**DECISION: implement the link manager as a MODULE in a SINGLE process** (behind
a C interface), NOT a separate daemon. A separate process would put an IPC
boundary inside the one-in-flight transaction discipline and force two event
loops to re-synchronize — synchronization issues for no gain at this tier.
Because the interface is identical, promotion to a process later (if ever) is a
packaging change, not a rewrite. The payoff today: all USB chaos
(DTR-reset-to-BOOT, ttyACM swaps, reconnect/resync) is quarantined in the link
manager; the portable brain stays clean and just sees link down→up.

---

## Phase A — Pi-side Layer-2 C foundation (against existing firmware)

**Step 0a — Link-endpoint interface + USB link manager.**
Define the `link endpoint` C interface. Implement the USB link manager beneath
it: enumerate/open CDC, SLIP framing, DTR handling, **link-down detection**
(device reset / read==0 / re-enumeration), reconnect with backoff. Exit: link
manager raises clean up/down events across a manual unplug/reset.

**Step 0b — Demux reader on top.**
One RX loop consuming frames from the link endpoint, dispatching by opcode:
reply (has `request_id`) → resolve pending; async (`OP_EVENT`/`OP_DBG_LOG`/
`OP_BUS_SLAVE_*`/`OP_HEARTBEAT`) → event router; unknown/stale → drop+log.
**Monotonic request_id allocation (never reuse).** Exit: one frame round-trips to
the BC and the reply is correctly correlated. *(Kills the id=64-reuse
mis-correlation class of bug.)*

**Step 1 — Find dongle + connect.**
Via the link manager: enumerate, open, identify role (REGISTER/sysinfo class_id).
Exit: C process attaches to the BC and reads its identity.

**Step 2 — Sync + resync robustness.**
Port the sync ladder (REGISTER_ACK→GET_MANIFEST→OPERATIONAL_BEGIN) into C. On a
link-up event from the manager, the controller re-runs the ladder automatically.
Exit: pull the BC's reset; the controller re-establishes OPERATIONAL with no
restart. *(Robustness the console never had; link-down lives in the manager,
re-sync-on-up lives in the controller — clean division.)*

**Step 3 — Roster load + recall.**
Layer-2 loads its authoritative roster from disk/config ("recall"), then pushes
the who-to-poll list to the BC (`CMD_BUS_REGISTER_SLAVE`). Two rosters: Layer-2
authoritative (class_ids, flags, persistence) vs. Layer-1 sweep list (addresses
only). Exit: cold-boot the BC; the controller re-populates the sweep roster.

---

## Phase B — Layer-1 ISR sweep (firmware rework)

**Step 4 — ISR empty-poll sweep, null messages.**
Move the sweep into the ISR on BOTH BC and slave: BC ISR cycles
POLL→(NO_MESSAGE/summary terminator)→next at window speed; slave RX ISR shifts a
pre-staged terminator. Liveness (ALIVE/DEAD) falls out; escalate DOWN/UP to
Layer-2 over the seam. Exit: BC sweeps a real slave + a phantom autonomously;
controller sees ALIVE + DEAD with **null traffic only**. *(The parked Stage-3a
logic splits here: sweep+roster+summary-bit stay on-chip; DOWN/UP/retry brain is
already up in Layer-2.)*

> Optional Step 3.5 spike (recommended): prove the bare ISR POLL→NO_MESSAGE
> turnaround timing on a scope/counter BEFORE the full sweep rework. Step 4 is the
> biggest single risk in the plan (ISR sweep timing on the SAMD21).

---

## Phase C — Content path (the normal AIs)

**Step 5 — Single TCP command→reply.**
Layer-2 injects one command into the sweep; slave executes via the shell layer;
reply rides its window back; demux resolves it. One in flight. Exit: bridged
echo/sysinfo to a slave, end-to-end, through the autonomous sweep.

**Step 6 — Command queue + scheduler.**
Bounded round-robin scheduler: multiple queued commands, one-in-flight
discipline, **Pi-only retry on timeout**, guaranteed per-slave visit interval.
Exit: queue N commands across M slaves; all complete; a dead slave times
out+retries without starving others.

---

## Phase D — Interlocks (the safety payload)

**Step 7 — Interlock async path.**
Summary-bit in the poll terminator → sweep escalates "slave N flagged" → Layer-2
requests detail (a *reliable* status read) → advisory notify to clients. Confirm
**local-acts-locally** holds (slave killed its own output on its ~1 kHz tick,
independent of all this). Exit: trip an interlock on a slave; Pi learns within
one bounded cycle; output already dropped locally.

---

## Phase E — Edge & error hardening

- Dongle reset mid-transaction (in-flight command survives via resync+retry).
- Slave dies mid-command (timeout→retry→DEAD).
- Bus CRC errors (drop + Pi re-ask).
- Bus-progress watchdog (wedged sweep bites even though engine ticks).
- TX-ring / queue backpressure (defer, never drop — the lesson from the
  2026-05-31 session).
- USB re-enumeration mid-run (link manager reconnect + controller resync).
- Multiple command sources contending on Layer-2.

---

## Open items

- **Test driver during Phase A/C:** a tiny C `main` driving the controller, with
  the LuaJIT console kept as a passive sniffer for observation. (Console stays a
  command source / service; it does not drive bring-up.)
- Step 3.5 timing spike: do it, or fold into Step 4.
