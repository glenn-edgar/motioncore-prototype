# Bus async command lifecycle (Step 6)

*Locked 2026-06-01 (design dialog). The command/reply model for the RS-485 bus:
ACK-frees-the-bus, async result, per-command timeout, and an L2
availability/completion tracker. Supersedes the synchronous in-window reply built
in Plan 1 Step 5. Companion to `docs/bus-controller-two-layer-architecture.md`
and `docs/plan-1-pi-bus-controller.md`.*

## 1. The problem with the synchronous path (Step 5)

Step 5 sent a command to a slave and **held the poll window open** until the
slave's reply came back. That couples *bus occupancy* to *command execution
time*: a slow HIL command (`adc_capture` ~300 ms) pins the bus for its whole
runtime, blocking the sweep and every other slave. It also caps throughput at
1 / round-trip and makes a slow command indistinguishable from a stuck one.

## 2. The model: ACK frees the bus; the result comes back async

> A command is **ACK/NAK-ed on receipt** by the slave's ISR — that frees the
> bus immediately. The slave executes on its own time. The **result** returns as
> a **separate reply** fetched on a later poll. A higher level (Layer 2) tracks
> each slave's availability and confirms the command actually completed.

Bus traffic is therefore only ever **short frames** — command + ACK, and later a
poll + reply — never the execution wait. Execution happens *off* the bus.

```
  L2 ──command(+timeout)──► BC ──DATA──► slave ISR: validate → ACK (bus free)
                                              │  (busy; executes in main loop)
  L2 ◄──reply──────────── BC ◄──reply──── slave: done → reply-ready → emitted on poll
```

## 3. Two timeouts — do not conflate them

| Timeout | Scope | Set by | On expiry |
|---|---|---|---|
| **ACK timeout** | bus-level, short (~ms) — receipt should be near-instant | fixed (firmware/L2 default) | Pi resends (bounded retries); then slave unreachable |
| **Execution timeout** | how long the command itself may take | **the API caller, per command** | command failed; if even slave's own bound passed, slave is *stuck* → flag it |

The caller knows its command's duration (`echo` → a few ms; `adc_capture` →
hundreds of ms), so it **states the execution timeout with the command**. No
per-command-class guessing in the tracker.

## 4. The per-command expected timeout travels on the wire — used at both ends

The command carries `expected_timeout_ms`. It is used twice:

- **Slave (self-abort):** the slave bounds its own execution by it. If a command
  overruns, the slave aborts it and **frees its busy slot** (error reply or just
  goes idle). This is load-bearing: it stops a hung command from pinning a slave
  *busy forever*, which would block all further commands to that node.
- **L2 tracker (completion deadline):** arms its deadline at
  `expected_timeout_ms + margin` (margin = wire + USB relay + slack), so the host
  never false-times-out *before* the slave's own bound. If even that expires, the
  slave is genuinely stuck → flag it (an availability/liveness concern, not just a
  command failure).

Wire encoding: the bus command frame carries `expected_timeout_ms` (u16) in its
header alongside `request_id` and `command_id`. (Exact layout to finalize at
build — e.g. `[request_id u16][command_id u16][expected_timeout_ms u16][args]`,
growing the bus-command header; the dongle-local USB shell path is unaffected.)

## 5. Slave side (Layer 1, in/near the ISR)

Per-node single-in-flight, enforced by the slave:

- **State:** `IDLE → BUSY(executing) → reply-ready → IDLE`.
- **On command receipt (RX-complete ISR):** CRC + address already validated by
  the assembler. If `IDLE` → accept into the input buffer, set `BUSY`, **ACK**
  (async TX from the ISR). If `BUSY` → **NAK** (can't take it now). NAK means
  "busy/can't accept", *not* "bad command".
- **Execute** in the main loop (`shell_dispatch`), bounded by
  `expected_timeout_ms` (self-abort).
- **On done:** fill the reply buffer, signal **reply-ready** in the poll status
  (a bit alongside the interlock summary-bit), and emit the reply on the next
  poll / reply-request. Return to `IDLE` once the reply is taken.

Builds directly on the 4b-i two-buffer ISR model (interlock buffer + reply
buffer, producers fill + mark fresh, ISR transmits). ACK/NAK use the existing
`RS485_FT_ACK`/`RS485_FT_NAK` frame types (the spec already says the slave
generates them for a master→slave DATA).

## 6. BC side (Layer 1)

The BC sweep is content-agnostic plumbing: it delivers the command DATA, relays
the slave's ACK/NAK up to L2, and — when a slave's poll status shows
**reply-ready** — fetches the reply (reply-request, or it rides the poll
response) and relays it up. The BC never waits for execution; it ACK-confirms and
keeps sweeping. Poll status carries both the **interlock summary-bit** and the
**reply-ready bit**.

## 7. L2 availability / completion tracker (the "higher level")

Per-slave state machine — the piece that "makes sure the slave did the command
response":

```
  IDLE ──send──► SENT(awaiting ACK, ACK-timeout armed)
   ▲              │ ACK                         │ NAK            │ ACK-timeout
   │              ▼                             ▼                ▼
   │           BUSY(exec deadline armed)   back off / requeue   resend (bounded)
   │              │ reply(matched by request_id)   │ exec-timeout
   └──────────────┘ fire on_done; slave IDLE       └─► command failed; slave stuck → flag
```

Responsibilities:
- **One command in flight per slave** — don't dispatch to a `SENT`/`BUSY` slave;
  the slave's NAK is the backstop if L2 and slave disagree.
- **request_id correlation** of the async reply to the originating command (the
  demux already does this).
- **ACK retry** (Pi-only, bounded) on ACK-timeout.
- **Execution timeout** per command (from the API value) → fail + flag.
- **Availability view** of every slave (idle/busy/stuck) for schedulers + clients.

## 8. Capacity (why ~40 cmd/sec multi-slave is comfortable)

Because execution is off the bus, each command costs the bus only a short
command+ACK now and a short poll+reply later (~tens of chars, ~ms at 115 k). 40
command-completions/sec across several slaves is a small fraction of the wire;
the real limiter is the L2 tracker + per-slave single-in-flight (so 40/sec is
spread across slaves, each serialized). A slow command no longer reduces
throughput — it just takes longer to *complete*, without holding the bus.

## 9. Relationship to the rest

- **Supersedes** Step 5's synchronous in-window reply.
- **Builds on** 4b-i (slave two-buffer ISR responses) and the summary-bit poll
  terminator (the reply-ready bit rides the same path).
- **This is Plan 1 Step 6**, expanded from "queue + scheduler + retry" into the
  full async command lifecycle. The bounded round-robin scheduler (interleave
  commands with guaranteed per-slave status visits) sits on top of this tracker.

## 10. To finalize at build

- Exact wire header layout for `expected_timeout_ms` (and whether a distinct
  bus-command opcode vs. growing `OP_SHELL_EXEC`).
- Reply-ready delivery: explicit reply-request vs. reply rides the next poll as a
  fresh buffer.
- ACK-timeout value + max retries; the L2 margin added to `expected_timeout_ms`.
- Slave self-abort mechanics for commands that can't be cooperatively cancelled
  mid-HIL (most are short; the few long ones need a checkpoint or a hard cap).
