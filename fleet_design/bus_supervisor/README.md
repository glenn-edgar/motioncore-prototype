# The Management Plane — a smart-satellite bus

This directory is the controller for a **management plane**: a low-speed,
out-of-band bus that runs *in parallel* to the fleet's high-speed data plane and
absorbs all the low-level work that has no business consuming high-speed-bus
bandwidth.

It is, conceptually, a **logical extension of a backplane SMBus** — the
housekeeping bus you find beside the main bus on a motherboard or backplane —
pushed off the board and out over RS-485 (and, later, over wireless) to reach
nodes the SMBus never could. The one twist that makes it more than SMBus: the
satellites on this bus are **full processors**, not dumb sensors, so a node can
run a closed loop or a real-time transform *locally* and report only conclusions
upstream.

---

## Two planes

```
        ┌─────────────────────────────────────────────────────────┐
        │  DATA PLANE  — high-speed fleet bus (Zenoh, :7447)        │
        │  primary robot operations, coordinated motion, app data   │
        └─────────────────────────────────────────────────────────┘
                          ║  (independent, parallel)
        ┌─────────────────────────────────────────────────────────┐
        │  MANAGEMENT PLANE — "extended SMBus" (this project)       │
        │  housekeeping · low-level I/O · health/maintenance DSP ·  │
        │  auxiliary actuators — none of it loads the data plane    │
        └─────────────────────────────────────────────────────────┘
```

The separation is the well-worn **management-plane vs data-plane** principle —
the same idea as a server's BMC/IPMI running beside the main CPU, or an
ATCA/MicroTCA **IPMB** management bus running across a backplane beside the
payload fabric. That precedent is reassuring: the architecture is sound and
proven. What is *ours* is the elevation of it (below).

### Why low-speed + polled is a feature, not a limitation

A management plane *should* be skinny. A slow, master-polled bus forces the
right discipline: **compute at the edge, send conclusions up.** The RA4M1 runs
its FFT/cepstrum locally and reports a band-power scalar (or paged bins on
demand); the motor "pseudo interlock" is polled rather than pushed. Nothing
heavy ever crosses the wire.

---

## The smart satellites

Plain SMBus/IPMB satellites are fixed-function management micros ("read register
0x12 off a thermistor"). Ours are general compute, each with a uniform
request-reply opcode API and a commissioned class+instance identity:

| Chip | Role on the management plane |
|------|------------------------------|
| **SAMD21** | General-purpose **housekeeping / HIL** node — GPIO, ADC, DAC, PWM, pulse counters, and the text-DSL **interlock** framework. Also runs the **bus-controller (BC)** role: USB-CDC ↔ RS-485 master. The management workhorse. |
| **RA4M1** | The **specialist** — real-time **spectral analysis for predictive maintenance** (Welch PSD, cepstrum, order-tracked Goertzel) *or* driving **auxiliary / side motors** (PID / S-curve / window-with-pinch). Dual-use: bench signal-processor **or** single-motor controller. |

The RA4M1 deploys in one of two patterns on the same firmware:
- **Monitor** — samples a signal/current from a primary machine and reports its
  *health* upstream, driving nothing. This turns the management plane's
  traditional "read a housekeeping sensor" role into genuine condition
  monitoring, computed entirely out-of-band.
- **Actuate** — drives its own aux/side motor that doesn't need to sit on the
  coordinated high-speed bus.

See `../../ra4m1/apps/register_dongle/` and `../../samd21/apps/register_dongle/`
for the per-chip firmware, and the memory notes `ra4m1-api-redesign-2026-06-03`
/ `ra4m1-resume-2026-06-03` for the RA4M1 control + DSP design.

---

## Dongle management + the Zenoh container

The bridge between the planes lives here. The supervisor is an Erlang-style
**`one_for_one` supervision tree on `chain_tree`** (LuaJIT), one supervised
child per logical dongle, each a non-blocking phase machine
(`wait_turn → open → provisioning → verify → serving`).

```
  WSL / any LAN host                 the Pi (robot)
  ┌───────────────┐   Zenoh RPC   ┌──────────────────────────────────────┐
  │ bus_cmd.lua   │──────────────▶│ container `btsup` (bus_supervisor:0.x)│
  │ bus_watch.lua │  tcp …:7448   │  ┌── own zenohd 0.0.0.0:7448 ───────┐ │
  │ selftest.lua  │◀──────────────│  │  (multicast OFF — isolated from   │ │
  └───────────────┘   replies     │  │   the fleet's data-plane :7447)   │ │
                                   │  └───────────────────────────────────┘ │
                                   │  chain_tree supervisor                 │
                                   │    │ libbus_controller.so (portable C) │
                                   │    ▼                                   │
                                   │  USB-CDC ──▶ SAMD21 BC ──▶ RS-485 ──▶  │
                                   │                              slaves    │
                                   └──────────────────────────────────────┘
```

Key properties:
- **Its own router, isolated.** The container runs its *own* `zenohd` on
  `0.0.0.0:7448` with multicast/scouting off, so the management plane never
  gossip-merges with the high-speed fleet's `:7447`. Either bus can be driven
  independently; either can be down without the other noticing.
- **Dongle = USB-CDC bridge; BC = RS-485 master.** A dongle relays named-JSON
  commands over Zenoh RPC → the portable C core (`libbus_controller.so`) →
  USB-CDC → the SAMD21 **bus controller**, which masters the RS-485 slave bus
  (9-bit MPCM addressing, implicit-token arbitration).
- **Identity-bound.** Each node is commissioned with a class + per-class
  instance, and dongles are **pinned by `chip_uid`** (USB serials are
  placeholders and `ttyACM*` re-enumerates across power cycles).
- **Serial bring-up + multi-dongle.** One supervisor serves N buses at once,
  brought up one-at-a-time behind a monotonic serial gate (verified N=2).
- **Drive it from anywhere on the LAN.** Point `ROUTER=tcp/<pi>:7448` at the
  container and run the operator tools (`tools/bus_cmd.lua`, `bus_watch.lua`,
  `selftest.lua`).

Build/run details: `packaging/README.md`. Live bench state, the self-test
plan, and resume notes: `CONTINUE.md`.

---

## Evolution: from a wired dongle to distributed wireless buses

The transport is deliberately abstracted behind `libbus_controller.so` and the
dongle role, so the *same* management-plane semantics can ride progressively
less-wired links:

1. **SMBus (on-board).** The origin: a backplane housekeeping bus.
2. **USB → RS-485 dongle (today).** The SMBus idea extended off the board — a
   USB-CDC dongle bridging to an RS-485 bus controller and its wired slaves.
   This is what runs on the bench now.
3. **Distributed wireless buses (roadmap).** The dongle becomes a *radio*, and
   the single wired bus fragments into many small wireless ones:
   - **Pico 2 W (RP2350 + CYW43 Wi-Fi)** hosting **remote dongles over Zenoh** —
     a dongle that lives wherever Wi-Fi reaches, joining the management plane's
     `:7448` Zenoh space directly instead of over USB.
   - **A Thread dongle + a Thread BC** — a low-power 802.15.4 **Thread** mesh
     segment (e.g. on a Thread-capable node such as the ESP32-C6 in the
     four-chip family), so leaf satellites form a self-healing wireless bus
     instead of a wired RS-485 trunk.

   The endpoint: the management plane stops being *one* USB+RS-485 dongle and
   becomes a **fabric of distributed wireless buses** — Wi-Fi/Zenoh segments and
   Thread mesh segments — all speaking the same commissioned opcode API, all
   still parallel to and isolated from the high-speed data plane.

The keystone that makes this migration cheap is the same one called out
elsewhere in the project: **`libcomm`/`libbus_controller` is the portability
layer.** A vendor sells you one good chip; here the chip (and increasingly the
*link*) is swappable while the API and commissioning model stay put.

---

## Honest positioning

- The **architecture has decades of precedent** (SMBus/PMBus, IPMI/BMC,
  ATCA IPMB, management-plane/data-plane separation). That validates it.
- What's distinctive is the **elevation**: *smart* satellites (general compute
  that closes loops and runs DSP at the edge), **extended reach** over RS-485
  and then wireless, a **uniform programmable API + commissioning across
  heterogeneous silicon** (SAMD21 / RA4M1 / RP2350 / ESP32-C6), and turning the
  management plane's housekeeping-sensor role into real **condition monitoring**
  (PSD / cepstrum / order-tracking) that never touches the data plane.
- Nobody sells *this* as a single product — a programmable, fleet-uniform
  smart-satellite management plane. The value is in the integration and the
  fleet/commissioning model, not in any one chip.

---

*Companion docs:* `CONTINUE.md` (resume + bench state + test plan) ·
`packaging/README.md` (image build/run) · firmware in `../../samd21/` and
`../../ra4m1/` · design memory `ra4m1-api-redesign-2026-06-03`,
`bus-session-2026-06-02-resume`, `four-chip-dongle-pivot-2026-05-11`.
