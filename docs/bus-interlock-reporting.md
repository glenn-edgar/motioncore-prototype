# Bus interlock reporting (L2)

*Locked 2026-06-01 (design dialog). How interlock state reaches L2 and clients,
now that an L2 layer exists. Heritage: the CANopen SYNC + EMCY split used by
CAN serial-link robot arms — best-effort async event for latency, periodic
broadcast for reliable refresh. Companion to
`docs/bus-async-command-lifecycle.md`,
`docs/bus-controller-two-layer-architecture.md` §7, and the
bus-async-and-interlock-model memory.*

## 0. The floor (unchanged): local-acts-locally

The interlock fires on the **slave**, at its ~1 kHz tick, and kills its own
output — independent of the bus, the BC, L2, the Pi. **L2 is never in the safety
loop.** Everything below is *observability + coordination*, advisory only; if the
bus and L2 vanish, slaves still self-protect (graceful degradation).

## 1. L2 holds a readable interlock buffer + state; clients pull it

L2 maintains, per slave, the latest interlock **state** (OK / tripped) and, when
tripped, the interlock **message** (which slot, inputs, detail). Clients
(console / AI / dashboard) **read** this cache over the command-source interface
whenever they want — they don't have to catch a transient push. L2 keeps it fresh
from the paths below.

## 2. Three reporting paths, three guarantees

| Path | When | Guarantee |
|---|---|---|
| **Per-poll summary-bit** | every poll terminator (1 bit) | continuous, near-free "is anything wrong right now"; drives fast notify + the `faulted` availability state |
| **Async interlock message** | on the trip edge, ride the slave's next poll response | low latency *when it lands* — best-effort/lossy, no retry (a corrupted/lost frame is gone until the next solicit) |
| **Periodic broadcast solicit** | configurable interval | reliable full refresh — guarantees L2's cache is re-asserted every interval even if every async message was lost |

Summary-bit = fast + cheap. Broadcast solicit = the reliable backstop. Together:
low latency when not lost, bounded worst-case = the broadcast interval.

## 3. Locked decisions (2026-06-01)

**D1 — Reporting path: summary-bit + broadcast.** Keep the 1-bit interlock
summary in *every* poll terminator (continuous, ~free) AND run the periodic
broadcast solicit for the reliable full state+message refresh.

**D2 — Solicit address: 0xFF broadcast.** The solicit is a frame to **dest
0xFF**; a slave accepts a frame when `dest == my_addr || dest == 0xFF`. 255 is
**reserved, never assignable to a slave**. Distinct from master (0x00), so a
slave never mis-reads another slave's `dest=0` response (which targets the
master) as a solicit. (0x00 was rejected for exactly that shared-bus ambiguity.)

**D3 — Solicit reply: full snapshot, clear on send.** On a solicit, **every**
slave reports its state once (OK or tripped); a tripped slave also includes the
message. The report-pending flag clears after that **one** response. So each
solicit produces a complete, reliable per-interval state map of all slaves (also
a liveness confirm). A lost solicited report isn't retried — it's simply
re-covered by the next solicit (the periodicity *is* the reliability).

## 4. Mechanics — collision-free on a multidrop bus

The broadcast does **not** trigger simultaneous replies (that would collide).
The 0xFF solicit **sets a `report-pending` flag on every slave**; each slave then
folds its state (+message if tripped) into its **next individual poll response**
as the round-robin sweep reaches it, and clears the flag. So:

```
  BC: broadcast solicit (dest 0xFF) ──► every slave sets report-pending
  BC sweep (serialized round-robin):
     poll slave A ──► A reports state[+msg], clears flag
     poll slave B ──► B reports state[+msg], clears flag
     ...                                  (one full refresh per sweep cycle)
```

The bus stays serialized; the broadcast is purely a synchronized *trigger* (the
SYNC analog). The async trip-edge message rides the same per-poll response path
(the slave's "interlock message" buffer in the two-buffer ISR model).

### report-pending flag lifecycle (slave)
`idle → (0xFF solicit received) report-pending → (folded into next poll response)
idle`. Idempotent: a second solicit before being polled just leaves it pending
(one report still covers it). Set in the RX ISR; cleared when the response is
emitted.

## 5. L2 reaction to a trip

1. **Update the readable cache** + mark the slave `faulted` in the availability
   tracker (still alive/polled, but tripped) — gates command policy.
2. **Notify clients** (advisory) over the command-source interface.
3. **Coordinated response** (if any): since there is **no slave↔slave** on this
   bus, bringing peers to safe states is **master-mediated** — L2 commands the
   peers via the normal async command path. This is **bounded-latency / advisory,
   not hard-real-time**; hard cross-slave safety must live in each slave's own
   local interlock. The *decision* of which peers to safe belongs to a
   **supervisor/policy layer above L2 core**, not the bus controller mechanism.
4. **Priority lane:** an interlock-driven detail read or safe-state command
   **preempts** normal per-slave command-queue traffic, and the scheduler's
   bounded round-robin **guarantees** the summary-bit sweep + solicit keep running
   (no flood of normal commands starves interlock reporting).

## 6. Configuration & tuning

- **Broadcast interval** (configurable) = the reliable-refresh period = the
  worst-case interlock-reporting latency. The async path covers latency *between*
  solicits when frames land.
- **Faulted-slave command policy** (default): hold the slave's normal command
  queue while tripped, allow safety/clear commands, surface the held backlog to
  the client, resume when the summary-bit drops.

## 7. Builds on / relationships

- **Built on:** the 4b-i two-buffer slave ISR (interlock message buffer + reply
  buffer), the summary-bit poll terminator (Step 7), the async command path +
  per-slave queue + availability tracker (Step 6, `bus-async-command-lifecycle`).
- **Refines:** `bus-controller-two-layer-architecture.md` §7 (adds the broadcast
  solicit + L2 readable buffer + the 0xFF address + full-snapshot decisions).
- **No slave↔slave** on this bus (master-mediated coordination only) — see the
  bus-async-and-interlock-model memory.

## 8. To finalize at build

- Solicit + report frame encoding (a distinct frame type for the 0xFF solicit;
  the report layout: `[state:u8]` always, `[message…]` when tripped).
- The slave RX-filter change to accept `dest==0xFF` in addition to `my_addr`.
- Where the broadcast-interval timer lives (reuse/extend the BC sweep timing).

## 9. Reconciliation model — index vs payload (refined 2026-06-01)

A sharper framing of §1–§4, settled in design dialog. It splits the reporting
into a **reliable index** (who is tripped) and a **lossy payload** (the message),
and makes the BC the authority that lets the Pi heal holes in the payload.

### The three parties

| Party | Holds | Reliability |
|---|---|---|
| **Slave** | the source of truth | on trip: sets the **bit** (in every poll) **and** pushes the **message** (buffer 2) |
| **BC** | each slave's current **status bit** | **reliable** — it polls every slave continuously, so it can never lose track of *who is tripped* (fresh within one poll cycle) |
| **Pi (L2)** | the **messages it actually received** | **lossy** — some buffer-2 messages were dropped (best-effort/UDP-style) |

The BC's picture (bits) is complete; the Pi's picture (messages) has holes.

### Bit = index, message = payload

- The **summary bit** is the reliable *index*: "slave N is tripped." It rides every
  poll (continuous) and is re-asserted by the BC→Pi status report (below).
- The **interlock message** (buffer 2: which slot, name, inputs, tf) is the *payload*
  — pushed by the slave on the trip edge, **best-effort, may be lost**.

### How the Pi heals holes: BC→Pi status report + reconciliation

The BC reports its **per-slave status** (the bits it already holds) to the Pi over
the **USB link** — periodically, and may also push on change. **This costs zero
RS-485 traffic** (the BC already has the status from the sweep; the report never
touches the bus). This report is the *truth reference* the Pi reconciles against:

```
for each slave:
  BC=TRIPPED & Pi has the message     -> consistent
  BC=TRIPPED & Pi has NO message      -> GAP (a buffer-2 message was lost)
  BC=ok      & Pi still shows tripped  -> clear (a recovery edge/message was lost)
```

A detected gap (BC bit says tripped, Pi is missing the message) is filled by the
Pi **requesting that one slave re-push its message** (a *targeted* re-solicit, not
a broadcast). So: the bit (reliable) tells the Pi *that* a message exists; the
reconciliation finds *which* are missing; a targeted re-push delivers them.

The BC is the authority **precisely because it polls everyone continuously**, and
it hands that authority to the Pi for free over USB. This supersedes the literal
"periodic 0xFF broadcast solicits every slave to re-report on the RS-485 bus" of
§2–§4 for the slow bus: the re-report the broadcast would trigger carries the same
*bit* the BC already holds, so re-asserting from the BC's copy over USB is strictly
cheaper. (The 0xFF broadcast mechanic remains the model for a future high-speed
tier where the master does **not** continuously poll every node.)

### Build order

1. **Slave buffer 2** — push the interlock message on the trip edge (the lossy
   payload). The 4b-i two-buffer ISR was specified for this but only the bit
   (Step 7) was built; this adds the message buffer. ISR transmits it on the next
   poll; BC relays it to the Pi.
2. **BC→Pi status report** — periodic per-slave status snapshot over USB (the
   reconciliation reference; zero bus traffic). Reuses 7a's L2 cache.
3. **Pi reconciliation** — diff BC-status vs received-messages; on a gap, request
   the missing slave's message (targeted re-push).
