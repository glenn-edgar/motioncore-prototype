# RS-485 bus protocol — BC-2 (CRC'd transport) + BC-3 (Pi-registered roster)

Status: **spec / proposed** (2026-05-29). Supersedes the un-CRC'd
`[addr][len][payload]` slice-1 framing once landed. Companion to the locked
bus-management design (memory `rs485-bus-management-design-2026-05-29`) and the
RS-485 slave protocol (`rs485-slave-protocol-2026-05-16`).

Goal: the bus_controller becomes a **general-purpose, topology-agnostic** bus
master. One BC binary everywhere; the Pi registers the slave roster at runtime
and the BC autonomously polls it. All bus frames carry a per-frame CRC.

---

## 1. Transport frame (replaces `[addr][len][payload]`)

Uniform structured header for **every** frame. 9-bit MPCM addressing is kept for
the cheap hardware address filter (the `dest` byte alone has the 9th bit set; all
following bytes are 8-bit data with the 9th bit clear).

```
  0xFF                preamble — RC/transceiver settle. NOT in CRC, NOT in len.
  [dest | bit8]       1B  destination addr, 9th bit set (MPCM match)
  [src]               1B  source addr (master = 0x00, slave = its addr)
  [type]              1B  frame type + flags (§2)
  [seq]               1B  sequence (TCP correlation; 0 on UDP frames)
  [len]               1B  payload length, 0..RS485_PAYLOAD_MAX
  [payload ...]       len B   (DATA frames only; see §3)
  [crc8]              1B  CRC-8/AUTOSAR over dest,src,type,seq,len,payload
```

* **Header overhead = 6 B** (dest+src+type+seq+len+crc) + 1 B preamble.
* **CRC** = `crc8_autosar()` from `vendor/libcomm/frame.c` (poly 0x2F, init 0xFF,
  final XOR 0xFF) over the 8-bit *values* of `dest..payload`. The 9th framing bit
  is not CRC'd. Reused as-is — no new CRC code.
* `dest = 0x00` = broadcast/master; `0xFF` reserved (sniffer). Slave addrs 1..254.
* Assembler state machine: `ADDR(bit8) → SRC → TYPE → SEQ → LEN → PAYLOAD[len] →
  CRC`. On CRC mismatch: drop; if the frame was TCP-class the receiver queues a
  NAK(seq). `len` overflow (>RS485_PAYLOAD_MAX) → drop + resync on next bit8.

### Control-frame size note

The old `no_message` was 2 B (its smallness mattered — it's the dominant bus
frame). Under the uniform header a control frame is **6 B** (zero payload). We
accept the +4 B for a single assembler path and uniform CRC integrity. At
115200 with N slaves polled, budget this into the poll rate. **Decision to
confirm:** uniform header (recommended) vs a dual-tier scheme that keeps
POLL/NO_MESSAGE at 2 B and CRCs only DATA/ACK/NAK.

---

## 2. Frame types (`type` byte)

```
  bits[3:0]  frame class
     0x0  NO_MESSAGE   slave→master: end-of-window terminator (UDP)
     0x1  POLL         master→slave: "your window is open" (UDP)
     0x2  DATA         carries a payload (§3)
     0x3  ACK          receiver: prior TCP DATA seq CRC-good
     0x4  NAK          receiver: prior TCP DATA seq CRC-bad → master retransmits
  bit[4]     TCP        1 = reliable (ack-tracked), 0 = UDP (fire-and-forget)
  bits[7:5]  reserved (0)
```

### Reliability model

* **TCP applies only to master→slave DATA.** Master assigns `seq`, tracks it in
  an ack-table. Slave validates CRC → ACK(seq) good / NAK(seq) bad, riding its
  next poll window. Master on NAK or response-timeout retransmits seq (max
  `tcp_retries`, default 3); exhausted → slave marked DEAD.
* **Slave→master is always UDP** (slaves are UDP-only, per the locked protocol).
  Master CRC-checks; a bad reply/event is dropped and the Pi's command simply
  times out and is retried whole. Keeps slave firmware trivial — no master-side
  ack to track.
* POLL / NO_MESSAGE are UDP. A corrupted POLL = one missed turn, recovered next
  round.

---

## 3. DATA payload = libcomm message (the transport seam)

A `DATA` frame's payload is a **libcomm message body prefixed by its 2-byte
opcode** — identical to what rides USB-CDC, minus SLIP + USB-CRC:

```
  payload = [opcode:u16-LE][body...]
```

The master demuxes by the *same* opcode space already used over USB:

| opcode | direction | body |
|---|---|---|
| `OP_SHELL_EXEC`  0x0109 | Pi→slave | `[request_id:u16][command_id:u16][args]` |
| `OP_SHELL_REPLY` 0x0011 | slave→Pi | `[request_id:u16][status:u8][result]` |
| `OP_EVENT`       0x0013 | slave→Pi | 6-B interlock edge body |
| `OP_DBG_LOG`     0x0010 | slave→Pi | UTF-8 text |

Max body = `RS485_PAYLOAD_MAX(120) − 2(opcode) = 118 B`. Replies > that clamp
(fragmentation is a later slice). This realizes the `comm_transport` keystone:
the BC just re-wraps the body for the Pi (USB-CDC + SLIP + USB-CRC) and vice
versa, no per-opcode translation.

---

## 4. Poll cycle (BC-3 autonomous master)

```
  for each ENABLED slave in roster (round-robin):
     if a queued Pi→slave TCP DATA for this slave exists: send it (seq, ack-table)
     else: send POLL(UDP)
     read the slave's window until NO_MESSAGE or response-timeout:
        ACK/NAK(seq)  → resolve ack-table entry
        DATA(UDP)     → forward body up to Pi (OP_SHELL_REPLY / OP_EVENT / OP_DBG_LOG)
        (timeout)     → consecutive_misses++ ; if ≥ max_misses → mark DEAD, push OP_BUS_SLAVE_DOWN
     on first good frame after DEAD → mark ALIVE, push OP_BUS_SLAVE_UP
```

The slave's RX-complete ISR transmits its pre-staged window (ACK/NAK from flash +
5-slot outbound queue drain + NO_MESSAGE) — zero compute in the ISR, per the
locked design.

---

## 5. Registration command set (Pi→BC, general-layer shell commands)

New `CMD_BUS_*` group at **0x0160** (clear of RS485 0x0150, interlock 0x0140).
Available only on `ROLE_BUS_CONTROLLER`. Invoked as `OP_SHELL_EXEC` bodies over
USB exactly like every other shell command.

```
  CMD_BUS_REGISTER_SLAVE   0x0160  args: addr:u8, class_id:u32, flags:u8
                                   reply: roster_count:u8   | NAK(roster_full/dup)
  CMD_BUS_UNREGISTER_SLAVE 0x0161  args: addr:u8            reply: roster_count:u8
  CMD_BUS_LIST_SLAVES      0x0162  args: —
                                   reply: count:u8, then per-slave:
                                     {addr:u8, class_id:u32, flags:u8,
                                      state:u8, misses:u8, last_seen_ms_ago:u16}
  CMD_BUS_SET_POLL         0x0163  args: poll_period_ms:u16, max_misses:u8,
                                         tcp_retries:u8       reply: —
  CMD_BUS_POLL_ENABLE      0x0164  args: enable:u8            reply: —
  CMD_BUS_CLEAR_ROSTER     0x0165  args: —                    reply: —
```

`flags` per slave: `bit0` = command this slave over TCP (else UDP), `bit1` =
enabled (polled). `class_id` is for the Pi's bookkeeping and an optional sanity
check against the slave's own REGISTER announcement.

### Roster (RAM-only)

```c
typedef struct {
    uint8_t  addr;                 // 1..254
    uint32_t class_id;
    uint8_t  flags;                // bit0 TCP, bit1 enabled
    uint8_t  state;                // UNKNOWN=0 / ALIVE=1 / DEAD=2
    uint8_t  consecutive_misses;
    uint32_t last_seen_ms;
} bus_slave_t;

#define BUS_ROSTER_MAX 16          // bounded; bus addr space is 254 but a real bus is small
static bus_slave_t g_roster[BUS_ROSTER_MAX];
```

**RAM-only by decision** — the Pi is the single source of truth. On BC cold boot
*or WDT recovery* the roster is empty; the Pi re-registers after it sees the BC's
`OP_REGISTER`. No flash wear, no stale-roster-after-rewiring bug, no second copy
to keep in sync. This couples cleanly to the just-verified WDT recovery: a BC
bite → reboot → empty roster → Pi re-pushes.

---

## 6. New s2m opcodes (BC→Pi async)

```
  OP_RS485_FRAME_RX  0x0014  (exists) raw sniff: [from_addr:u8][payload]
  OP_BUS_SLAVE_DOWN  0x0015  [addr:u8]                — slave missed max_misses
  OP_BUS_SLAVE_UP    0x0016  [addr:u8][class_id:u32]  — slave recovered
```

Forwarded slave replies/events reuse their own opcodes (`OP_SHELL_REPLY`,
`OP_EVENT`, `OP_DBG_LOG`) re-emitted on USB, tagged with the slave addr in the
frame `addr` field — exactly as the BC-1b bridge already does.

---

## 7. WDT coupling (do NOT defer)

Autonomous polling (§4) invalidates "WDT proves the engine ticks." A wedged poll
loop with a still-ticking engine would not bite. So the design's **"pet only on
actual byte progress"** bus-progress watchdog ships *with* BC-3, not after:
gate the WDT pet on RS-485 RX/TX byte progress within a bounded silent-wait, so
a stuck bus trips recovery while a healthy idle poll loop does not. (Verified
foundation: both roles bite+recover, memory `rs485-bus-management-design`.)

---

## 8. Build slices

This is a **coordinated wire-format migration** — BC, slave, and the dongle
sniffer all change together; the old 2-/3-byte framing is dropped.

1. **Transport rewrite** (`samd21_rs485.c/.h`): new header + CRC in
   `rs485_send_frame()` (now takes `src,type,seq`) and the assembler in
   `rs485_poll_frame()` (now returns `src,type,seq` too). Add `crc8` check.
2. **Slave** (`main.c rs485_slave_poll`): parse type; DATA→dispatch by inner
   opcode; emit ACK/NAK for TCP; reply/event as UDP DATA; NO_MESSAGE terminator.
3. **BC roster + commands** (`samd21_commands.c`): `CMD_BUS_*` + `g_roster` +
   `bus_roster.c` helper.
4. **BC poll engine** (`main.c`): round-robin (§4), ack-table, dead detection,
   `OP_BUS_SLAVE_*` push. Bus-progress WDT pet (§7).
5. **dongle_console.lua**: `--send-shell-bus-register` etc., `OP_BUS_SLAVE_*`
   decode, roster pretty-print.
6. The BC-1b synchronous targeted bridge (`--target <addr>`) stays — it becomes
   "send a queued TCP DATA on the next poll slot for that addr" rather than an
   immediate one-shot.

---

## 9. Open decisions

* **§1 uniform 6-B header vs dual-tier** (keep POLL/NO_MESSAGE at 2 B). Lean:
  uniform.
* **Roster size** `BUS_ROSTER_MAX` — 16 enough for the bench? (addr space is 254).
* **How the Pi learns the roster**: config/commissioning-DB driven (default) vs a
  BC `bus_scan` discovery command (probe 1..254, report responders) for
  plug-and-play. Lean: config-driven now, scan later.
* **BC-2 / BC-3 ordering**: spec'd together here so the CRC'd header is cut once
  and the roster rides it from day one (avoids building on the un-CRC'd frame).
