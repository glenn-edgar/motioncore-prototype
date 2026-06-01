# bus_controller — Pi-side Layer-2 bus controller (Plan 1)

Portable C implementation of the **Layer-2 bus controller** (the "brain"): roster,
scheduler, demux, retry, and the command-source interface. It drives a SAMD21
RS-485 bus over USB-CDC today, and is written so the same core routine relocates
to an M33/M7 dongle or a Zenoh node later (only the link impl below the seam
changes).

See the design docs:
- `docs/bus-controller-two-layer-architecture.md` — the two-layer model + invariant
- `docs/plan-1-pi-bus-controller.md` — the build sequence (phases A–E, steps 0a–7)

## Layering

```
  procedure shell   (per-platform: an OS process on Linux, one per dongle, keyed by dongle id)
    └─ core C routine     (portable Layer-2 brain: roster·scheduler·demux·retry)
         └─ link endpoint  (the seam — link_endpoint.h)
              └─ USB link manager   (usb_link.c — SLIP+CRC, DTR, reconnect; Pi tier only)
```

- **`link_endpoint.h`** — the tier-portability seam. Deals in *decoded frames*
  (`frame_meta_t` + payload), so SLIP/CRC framing never leaks above it. Same
  interface on every tier; only the impl below changes.
- **`usb_link.{h,c}`** — the Pi-tier impl. The ONLY place USB chaos lives:
  enumerate/open CDC, termios, DTR, link-down detection, reconnect backoff, and
  SLIP+CRC via the vendored `libcomm`. One instance owns one device.
- **`vendor/libcomm/`** — a copy of the SAME frame codec the firmware runs, so the
  USB wire format matches by construction. Do not edit here; re-lift upstream.

## Build & run

```sh
make                   # builds all step drivers
./step0a [/dev/ttyACMx] # link manager bring-up: prints UP/DOWN + decoded frames
./step0b [/dev/ttyACMx] # demux: walks a minimal sync ladder, sends CMD_ECHO,
                        #   correlates the reply by request_id, prints PASS/FAIL
```

`step0a` opens a dongle, prints every link UP/DOWN edge and every decoded inbound
frame, and survives a manual unplug/reset (DOWN→UP, no restart). It sends nothing;
the SAMD21 firmware's own REGISTER/HEARTBEAT traffic exercises the decode path.

`step0b` adds the demux (`demux.{h,c}`): one RX path dispatching `SHELL_REPLY` to a
pending request (correlated by request_id) vs. everything else to an async router.
The inline sync ladder it walks is throwaway test scaffolding — Step 2 builds the
robust version in the controller.

`step1` starts the reusable **controller core** (`controller.{h,c}` + `identity.{h,c}`):
opens a dongle, captures its REGISTER announcement, and prints the decoded identity
(role from class_id, instance, commissioning, fw version, UID). Steps 2–3 grow this
same controller object.

`step2` lets the controller drive the sync ladder to OPERATIONAL itself and
auto-resync after a dongle reset (no program restart).

`step3` loads the Layer-2 authoritative roster (`roster.{h,c}`, e.g.
`rosters/example.conf`) and, once OPERATIONAL on a BC, auto-pushes the sweep list
down via `CMD_BUS_*` (CLEAR → REGISTER×N → SET_POLL → LIST), then reads it back.

## Status

- **Step 0a — link-endpoint seam + USB link manager: DONE.** Hardware-verified on
  the Pi: cold-start scan, UP on plug-in, clean SLIP+CRC decode, and clean DOWN→UP
  across resets with no program restart.
- **Step 0b — demux reader: DONE.** Hardware-verified: CMD_ECHO round-trip
  correlated by request_id, payload verified byte-for-byte, async stream
  (REGISTER/HEARTBEAT/DBG_LOG/MANIFEST_REPLY) cleanly separated, monotonic
  request_id (unknown-id replies dropped, not mis-latched).
- **Step 1 — find dongle + connect, identify role: DONE.** Hardware-verified:
  controller captured REGISTER in BOOT and decoded full identity (role from
  class_id, instance, commissioning, fw version, UID). Reusable `controller`/
  `identity` core established.
- **Step 2 — sync ladder + auto-resync: DONE.** Hardware-verified on both roles
  (slave inst 1, bus_controller inst 42): controller drives
  BOOT→L1_ACKED→MANIFEST_OK→OPERATIONAL itself, captures the manifest
  (schema_hash 0x80AEB146), and on a dongle reset re-runs the ladder back to
  OPERATIONAL with no program restart — riding the USB re-enumeration via the
  link manager's scan mode. Self-healing: every REGISTER re-triggers the ACK.
- **Step 3 — roster recall + push: DONE.** Hardware-verified on the BC (inst 42):
  loaded a 3-slave roster from disk, auto-pushed CLEAR→REGISTER×3→SET_POLL→LIST
  via chained CMD_BUS_* replies, and read it back (all 3 present, state=UNKNOWN —
  polling not yet enabled). Role-gated to bus_controller. Reprovision-on-reset
  rides the Step-2 resync path. End of Phase A.
- **Step 4 — autonomous sweep + liveness escalation: DONE (point-to-point TTL).**
  Hardware-verified BC↔slave over a bare-TTL cross-wire (D6↔D7, no transceivers):
  controller enables the BC's main-loop poll sweep, slave answers POLL→NO_MESSAGE,
  and the full liveness cycle escalated to L2 in one run — ALIVE (seen≈20ms) →
  unplug → DEAD at max_misses → `OP_BUS_SLAVE_DOWN` → replug → ALIVE →
  `OP_BUS_SLAVE_UP`. (The parked Stage-3a firmware is the main-loop sweep; the ISR
  migration is the deferred Step 4b.) Provisioning now gated on identity so an
  already-OPERATIONAL attach can't spuriously FAIL.
- **Step 5 — content command→reply through the sweep: DONE (point-to-point TTL).**
  Hardware-verified: while the liveness sweep runs, the BC injects a Pi command
  into a poll slot as DATA (firmware "Stage 3c"), the slave executes it, the reply
  rides the same window back, and the demux correlates it by request_id. 5/5
  CMD_ECHO round-trips to the slave verified byte-for-byte with liveness polling
  concurrent. One command in flight; queue + scheduler + retry is Step 6.
  Completes the full one-slave vertical run.
- **Capstone Phase 1 — fake-console API suite: DONE (point-to-point TTL).** A C
  "fake console" (capstone_main.c) brings the BC to OPERATIONAL, provisions +
  enables the sweep, then runs the API command suite to the slave through the
  sweep: echo, sysinfo, stack_hwm, and the analog loopback DAC(A0)->ADC(A1)≈4×
  (via the A0↔A1 jumper) — 4/4 pass. (i2c_scan omitted: blocks on bare pins.)
- **Step 7 — summary-bit interlock escalation: DONE (point-to-point TTL).** Slave
  puts a 1-byte summary in its poll terminator (bit0 = an armed interlock tripped);
  BC tracks it per-slave and escalates the edge as OP_BUS_SLAVE_FLAGGED
  (defer-never-drop); L2 surfaces it via a flagged callback.
- **CAPSTONE — fake console, full stack: 10/10 PASS (point-to-point TTL).** One
  run: API suite (echo, sysinfo, stack_hwm, DAC→ADC loopback) + interlock arm →
  trigger by driving the DAC into the watched ADC (self-triggering via the A0↔A1
  jumper) → slave trips locally + sets summary-bit → BC escalates SLAVE_FLAGGED →
  L2 sees tripped, INTERLOCK_STATUS tf=2, then clears on recovery → disarm. The
  full BC stack + slave stack, end to end.
- Next (per plan): add RS-485 transceivers (MAX485, 4-wire — same protocol/fw),
  re-run the capstone over the real bus, then add more slaves. Then Step 6
  (queue + scheduler + retry) for multi-slave fairness.

## Known follow-ups
- **Device pinning:** with multiple dongles, scan-grabs-first-`/dev/ttyACM*` is
  ambiguous after a re-enumeration swap. Pin each controller to a stable
  `/dev/serial/by-id/...` path keyed by dongle id / chip serial when packaging the
  per-dongle procedure shell (see docs/plan-1-pi-bus-controller.md).
