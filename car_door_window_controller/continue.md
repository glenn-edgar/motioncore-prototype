# continue.md — Car Window Controller (CWC)

**Form factor:** a virtual-robot **class** that drops into the slave-side libcomm / FreeRTOS-SMP infrastructure described in `motioncore-prototype/continue.md` and `dongle_libcomm_pico_port_plan.md`.
**Target slave hardware:** RP2350 (Pico 2 W) with a CWC carrier board (DRV8838 H-bridge, INA240 current sense, N20 worm-drive motor, quadrature encoder, vehicle-side CAN via can2040 PIO, debug UART).
**Status:** second-pass spec — post-pivot to slave-infra integration, with bench-shell architecture for pre-infra bring-up.
**Philosophy:** hard-RT internal, bus_msg_t-driven externally. No soft-fallback pre-fault — hard-RT or `reboot_with_cause()`. Bench shell exists for hardware bring-up and motor characterization while infra is in development.

---

## 0. Scope & relationship to slave infrastructure

The dongle and slave infrastructure are developed first (parent project P1–P5). The CWC is one class registered into that already-running framework. It does not duplicate any infra responsibility.

### Provided by slave infra (not in this spec)

| Concern | Owner |
|---|---|
| Wire framing, addressing, CRC, transport — both RS-485 uplink to dongle and CAN to peripherals | libcomm + `ext_bus_pio_rs485` and `ext_bus_can` threads |
| `bus_kernel_pico` primitives — task / msgq / timer / mutex | shared `firmware/pico/common/bus_kernel_pico.c` |
| Class registration, generic `main()`, manager + internal_bus | slave skeleton (parent P4) |
| Deadline supervisor / hardware watchdog / reboot_cause capture | guardian + `reboot_cause.{h,c}` (parent P5) |
| Reboot-loop / degraded-mode handling | infra (parent — universal across classes) |
| ChainTree / s-expr C runtime (already in C) | `firmware/pico/common/chaintree_runtime.{h,c}` |
| Persistent storage (LittleFS) — used only for commission record (identity) | parent P2 |
| Health-snapshot telemetry — auto-reports CWC's queues/tasks once registered | parent P8 |
| Boot-announce frame (carries `reboot_cause` + scratch[1..3]) | infra |
| `infra_register_shutdown_handler()` API — hook called before any software-triggered reset | infra |

### Owned by the CWC class (this spec)

- Motor-control pipeline: bridge driver, ADC + current sense, quadrature encoder, ripple analysis, S-curve trajectory, open-loop torque feedforward, end-stop & anti-pinch detection.
- Three application threads — `cwc_outer`, `cwc_inner`, `cwc_supervisor` — plus one auxiliary thread `cwc_dbg_drainer`.
- ChainTree behavior program compiled into firmware via `binary.h` (no runtime loading).
- Debug telemetry ring + UART1 drainer.
- Bench shell — pre-infra bring-up and motor characterization framework.
- CWC opcode set consumed via bus_msg_t (movement, calibration, diag readback, live-tune of whitelisted symbols).
- Symbol namespace under `cwc.*` registered with the shared symbol table.
- Class-specific reboot-cause codes (≥ `USER_BASE = 64`).

### Class entry point

```c
void cwc_register(internal_bus_t *ibus,
                  manager_t      *mgr,
                  const cbor_t   *class_config,  // from /commission.cbor — identity + pin overrides only
                  uint16_t        instance_id);
```

`cwc_register` instantiates the four CWC tasks, allocates queues, registers them with the deadline supervisor, registers the CWC opcode handler with `manager`, registers `cwc.*` symbols with the shared table, registers `cwc_emergency_shutdown` with the infra shutdown handler chain.

After registration, the slave skeleton owns lifecycle. Once an `OP_CWC_STATE_READY` arrives from the host, the CWC transitions out of INIT.

---

## 1. Hardware BOM

| Block | Part | Notes |
|---|---|---|
| MCU | RP2350 (Pico 2 W) | dual M33, FPU, DSP ext, 3× PIO, 520 KB SRAM, 4 MB flash. **WiFi/CYW43 not used** — slave is RS-485+CAN only. |
| H-bridge | TI DRV8838 | PHASE/ENABLE, slow-decay, 1.8–11 V, 1.8 A. No nFAULT pin (status inferred). |
| Current sense | TI INA240A4 | gain 200 V/V, V_S = 3.3 V, REF = GND (unidirectional, high-side) |
| Shunt | 100 mΩ, ≥0.5 W, 1% | between V_M and DRV8838 VM |
| Motor | N20 brushed DC, 238:1 gearbox | 7 pulses/motor-rev encoder, 3-pole armature assumed (verify with scope) |
| V_M supply | 6 V regulated | I_stall ≈ 125 mA |
| Vehicle CAN PHY | TJA1050 (or equivalent) | PIO-driven soft-CAN (can2040 pattern) — pins owned by slave infra, not CWC |
| Debug UART | USB-UART adapter (FT232 / CP2102) | bench-side connection to UART1; 1 Mbaud |
| Misc | fault LED on GP22 | thermal vias under DRV8838 pad |

### Motor operating point (specified by user, verified on bench during characterization)

- No-load current: ~20 mA at 12 V (~10 mA at 6 V).
- Loaded running current: ~40 mA at 12 V (~20 mA at 6 V).
- Stall current: ~250 mA at 12 V (~125 mA at 6 V).
- Winding resistance: R ≈ 48 Ω (from 12 V / 250 mA stall).

At 6 V operating voltage, INA240A4 with 100 mΩ × 200 V/V = 20 V/A scaling:
- 10 mA no-load → V_OUT = 0.20 V (6 % of 3.3 V range)
- 20 mA loaded → V_OUT = 0.40 V (12 %)
- 125 mA stall → V_OUT = 2.50 V (76 %)

Comfortable headroom for reversal transients within the 3.3 V ADC range.

End posts: foam, deformable, ~10–20 N compression force. Both home reference and over-travel limit.

### Pull-down requirements (safety-critical)

Detailed in `hardware_notes.md`. Summary: **10 kΩ pull-downs on GP10 (PHASE), GP11 (ENABLE), and GP12 (nSLEEP)**, located physically close to the DRV8838. These are the only protection against motor motion during hardware-triggered resets (watchdog, brown-out, power-up rail rise), since no software runs at those points.

### Hardware shared with slave infra (not CWC-owned)

- RS-485 PHY for uplink to dongle.
- CAN PHY (TJA1050) for can2040 soft-CAN.
- LittleFS-backed flash region (last 64 KB).
- USB peripheral (unused on slave — keep BOOTSEL functionality only).

---

## 2. Pin map (Pico 2 W)

CWC-owned pins listed below. Infra-owned pins (RS-485 uplink, CAN PHY) are negotiated at integration; CWC must not contend.

| GP# | Function | Notes |
|---|---|---|
| GP4 | **UART1 TX** → debug host | 1 Mbaud, 8N1. Owned by `cwc_dbg_drainer` |
| GP5 | **UART1 RX** ← debug host | shell input (initially) |
| GP10 | DRV8838 PHASE | 10 kΩ pull-down required |
| GP11 | DRV8838 ENABLE | PWM out, 10 kΩ pull-down required |
| GP12 | DRV8838 nSLEEP | 10 kΩ pull-down required |
| GP14 | Encoder A | PIO quadrature input, internal pull-up enabled |
| GP15 | Encoder B | PIO quadrature input, internal pull-up enabled |
| GP22 | Fault LED | pull matches LED polarity |
| GP26 / ADC0 | INA240 V_OUT | current sense |
| GP27 / ADC1 | V_M divider | supply-voltage feedforward |
| GP28 / ADC2 | spare (temperature sensor optional) | pull-down if unpopulated |

The onboard LED on Pico 2 W is wired through CYW43, not GP25 — do **not** use it for status. GP22 fault LED is the only LED CWC drives.

### PIO allocation (CWC-owned slices)

- **One SM:** PWM trigger generator + ADC sample trigger (sync ADC to ~70 % of PWM ON window).
- **One SM:** quadrature encoder decoder (32-bit signed accumulator).

CWC requests two SMs from a single PIO instance at registration time. Default: PIO0 SM0 + SM1. Remaining PIO bandwidth (PIO0 SM2-3, all of PIO1 and PIO2) is available for slave infra (RS-485 half-duplex, can2040 soft-CAN, etc.).

### DMA channel allocation (CWC-owned)

- One channel for ADC round-robin → SRAM ring.
- One channel for UART1 TX from debug ring.

RP2350 has 16 channels; CWC uses 2. Infra has comfortable headroom.

---

## 3. Architecture

### 3.1 Tasks

| Task | Core | Priority | Driver | Role |
|---|---|---|---|---|
| `cwc_inner` | 1 | **HIGH** | DMA-complete IRQ (32-sample ADC blocks @ 25 kHz, ~1.28 ms cadence) | ADC0+ADC1 round-robin consumption, ripple HPF + Goertzel + PLL, direction-segregated Welford, open-loop torque feedforward → PWM duty, end-stop/obstruction primitive detectors |
| `cwc_outer` | 0 | **MED+** | 1 ms tick + inbox events | State machine (INIT/HOMING/IDLE/MOVING/RETRACT/FAULT), S-curve trajectory generator, directive dispatch from supervisor, fault arbitration, state-delta event emission to host |
| `cwc_supervisor` | 0 | **MED** | timer (configurable: 10 ms or 100 ms) | ChainTree program tick — mode arbitration, condition monitoring, fault escalation policy, vehicle/host opcode policy. Snapshot → eval → dispatch model |
| `cwc_dbg_drainer` | 0 | **LOW** | UART1 TX-done IRQ + producer notification | DMA-pumps the debug ring to UART1 TX. Does not register with deadline supervisor |

Priorities chosen so cwc_outer (which feeds inner at 1 kHz) sits above cwc_supervisor; supervisor runs interpreted code so its variability is isolated from outer's deterministic ticks.

Deadline-supervisor budgets:

| Task | `tick_period_ms` | `max_acceptable_age_ms` |
|---|---|---|
| `cwc_inner` | 1 (rounded from 1.28) | 3 |
| `cwc_outer` | 1 | 4 |
| `cwc_supervisor` | 10 or 100 (configurable) | 4× tick_period |

Miss → `reboot_with_cause(REBOOT_CAUSE_DEADLINE_MISS, ...)`. No degraded mode pre-fault.

### 3.2 Queues (`bus_msgq_t`, libcomm-style)

| Queue | Producer → Consumer | Depth | Overflow | Carries |
|---|---|---|---|---|
| `outer→inner setpoint` | `cwc_outer` → `cwc_inner` | 2 | REBOOT | `setpoint_t` |
| `inner→outer telemetry` | `cwc_inner` → `cwc_outer` | 2 | REBOOT | `inner_telemetry_t` summary |
| `supervisor→outer directive` | `cwc_supervisor` → `cwc_outer` | 4 | REBOOT | resolved motion / mode directive |
| `outer→supervisor summary` | `cwc_outer` → `cwc_supervisor` | 4 | REBOOT | per-traverse summary, fault notifications |
| `manager→supervisor inbox` | slave `manager` → `cwc_supervisor` | 4 | REBOOT | host + vehicle opcodes |
| (debug ring) | any producer → `cwc_dbg_drainer` | 32 KB | **DROP** | telemetry records (not control data) |

REBOOT-on-overflow is infra default for control queues. Debug ring is the explicit exception — telemetry loss is acceptable, control loss is not.

### 3.3 Inner ↔ outer payloads

Both fit within a 32-byte `bus_msg_t` inline payload:

```c
typedef struct {
    uint32_t seq;
    int32_t  p_d;            // desired position, quadrature counts
    float    v_d;            // counts/sec
    float    a_d;            // counts/sec²
    uint8_t  mode;           // SETPOINT_IDLE / _RUN / _BACKLASH_CROSS / _CREEP / _BRAKE / _COAST
    int8_t   dir;            // -1, 0, +1
    uint16_t flags;
} setpoint_t;                /* 24 B */

typedef struct {
    uint32_t seq;
    int32_t  pos_motor;      // raw quadrature
    int32_t  pos_output;     // backlash-compensated
    int16_t  v_meas_q8;      // counts/sec (Q8)
    uint16_t i_mean_mA;
    uint16_t i_ripple_mA;
    uint16_t ripple_freq_q4; // Hz (Q4)
    uint16_t fault_flags;
    uint16_t sub_state;
} inner_telemetry_t;         /* 24 B */
```

Full-precision floats and long arrays (Welford state, ripple spectrum, ident params) are delivered on demand via diagnostic opcodes or via the debug ring — never on the periodic feed.

### 3.4 Data flow

```
ADC0+ADC1 round-robin ──DMA──► cwc_inner
                                  │ ripple + Welford + FF
                                  ▼
                              PWM duty write (GP11)
                                  │
                                  └──inner_telemetry_t──► cwc_outer
                                                            │
manager (host+vehicle opcodes via infra) ──► cwc_supervisor.inbox
                                                            │
                                  cwc_supervisor ──directive──► cwc_outer
                                                            │
                                  cwc_outer ──setpoint_t──► cwc_inner

                                  cwc_outer ──summary──► cwc_supervisor
                                  cwc_supervisor ──event/state-delta──► manager

Any producer ──record──► debug ring ──DMA──► UART1 TX (cwc_dbg_drainer)
```

CWC never speaks libcomm framing; it produces and consumes opaque `bus_msg_t` opcodes routed by the slave's `manager`.

---

## 4. Subsystems

### 4.1 Bridge driver (DRV8838)

| nSLEEP | ENABLE | PHASE | Result |
|---|---|---|---|
| 0 | x | x | Coast (Hi-Z) |
| 1 | 0 | x | Brake (low-side short) |
| 1 | PWM | 0 | Drive reverse, slow-decay |
| 1 | PWM | 1 | Drive forward, slow-decay |

- PWM on ENABLE, 25 kHz, 12-bit duty resolution.
- PHASE held during PWM cycles; flipped only at zero-velocity transitions.
- **Direction reversal:** brake (5 ms) → coast (`nSLEEP=0`, 5 ms) → flip PHASE → re-enter drive via backlash sub-profile. Never flip PHASE under PWM.
- **COAST in IDLE:** the worm is non-back-drivable. After a successful end-stop arrival or `IDLE` transition, drop `nSLEEP = 0` to coast. Window holds mechanically at ~µA quiescent current. State machine wakes back to brake-then-drive on next motion command.

### 4.2 ADC + current sensing (INA240A4 + V_M divider)

**Single PIO trigger captures both channels via round-robin:**

```c
adc_set_round_robin((1u << 0) | (1u << 1));   // ADC0 (INA240), ADC1 (V_M)
adc_fifo_setup(true, true, 1, false, false);  // DMA + DREQ, one sample per request
```

PIO triggers ADC at ~70 % of PWM ON window. ADC converts ADC0 then auto-advances to ADC1; both samples land in a DMA ring within ~4 µs (well inside the 40 µs PWM cycle). Ring is 128 sample pairs deep; `cwc_inner` consumes 32-pair blocks (1.28 ms latency).

Current path consumes `ring[2k]`; V_M path consumes `ring[2k+1]` and applies an EMA (τ ≈ 50 ms) before use in feedforward.

**No sampling during PWM OFF window** — slow-decay brake bypasses the high-side shunt; samples would be zero and dilute Welford stats. PIO trigger guarantees ON-window placement.

Overrange handling: discard. Persistent overrange (≥3 in any 32-sample block) → `FAULT_OVERCURRENT`.

### 4.3 Encoder

**Counting mode: quadrature, 4× edges.** Both A and B wired; PIO program decodes all four edge transitions.

- 7 pulses/motor-rev × 4 edges × 238:1 = **6664 counts per output revolution**.
- 270° travel ≈ **5000 counts**.
- PIO publishes count into a shared atomic `int32_t` polled by `cwc_outer`.
- **Encoder stall watchdog:** `|Δcount|` over 100 ms < 4 quadrature counts while `|duty| > 10 %` → `FAULT_ENCODER_STALL`.

If B-channel anomaly detected at boot self-test, fall back to single-edge with commanded-direction signing; rescale profile constants by ÷4; raise `WARN_ENCODER_DEGRADED`.

### 4.4 Ripple analysis

Brushed-DC commutation ripple = N_poles ripples per motor revolution. N_poles assumed 3 (verify with scope; sweep {3, 5} during ident). Band: ~0 Hz stalled to ~500 Hz running. 25 kHz sample rate gives Nyquist 12.5 kHz — comfortable.

**Pipeline (cwc_inner, 25 kHz sample-driven):**

1. **High-pass IIR** on raw current, 1st-order, fc ≈ 20 Hz → ripple-only signal `r[n]`.
2. **Goertzel** at tracking f₀, block size 64 samples (2.56 ms). Outputs `R_mag`, `R_phase`. **Freezes PLL when `R_mag < threshold`** to prevent noise-driven drift at low duty / stall.
3. **PLL:** phase-detector = R_phase derivative; PI loop filter. Bound f₀ ∈ [10, 800] Hz, slew limit ±200 Hz/s.
4. **Ripple zero-cross counter:** debounced by ¼-cycle hold-off; signed by `dir`.

**Why both ripple AND mean-current detection for end-stop / obstruction:** ripple goes to zero in ≤ one period (~7 ms at running speed) when the motor stalls — 5–10× faster than mean current can rise (30–100 ms through L/R time constant). Ripple-stall is the fast path; mean-current is the confirmation. This is what makes anti-pinch detection meet FMVSS-118-style budgets.

**Cross-check:** expected ratio `ripple_per_encoder ≈ N_poles / (7 × 4) ≈ 0.107` (quadrature). Five-sample moving ratio deviating > 25 % for > 500 ms during drive → `FAULT_ENCODER_RIPPLE_MISMATCH`. Resolve by majority vote with prior known-good cycle.

### 4.5 S-curve trajectory generator (cwc_outer, 1 kHz)

Jerk-limited 7-segment profile, reentrant, supports mid-profile retargeting (for obstruction-reverse).

```c
typedef struct {
    float v_max;             // counts/sec
    float a_max;             // counts/sec²
    float j_max;             // counts/sec³
    float v_creep;           // final approach
    int32_t decel_margin;    // counts before expected end-stop where decel completes
} traj_profile_t;

traj_profile_t profile_up, profile_down;
```

**Defaults (quadrature counts):**
- `v_max` ≈ 3200 cps (full 270° in ~1.5 s)
- `a_max` ≈ 16000 cps²
- `j_max` ≈ 160000 cps³
- `v_creep` ≈ 160 cps
- `decel_margin` ≈ 50 counts

Sanity: `v_max² / a_max = 640 < 5000 (travel)` → cruise phase reachable.

The J⁺ phase of the S-curve **is** the soft-start. No separate ramp logic — duty rises smoothly from zero because a_d and v_d both start at zero. Eliminates the "first-100-ms inrush masks pinch" blind spot that catches naïve open-loop controllers.

### 4.6 Open-loop torque feedforward (cwc_inner)

```
τ_d   = J · a_d + τ_friction(v_d, dir)
i_d   = τ_d / Kt
V_cmd = R · i_d + Ke · v_d
duty  = clamp(V_cmd / V_M_measured, -DUTY_MAX, +DUTY_MAX)
PHASE = sign(duty); PWM = |duty|
```

Coefficients from `ident_params` (host-pushed). `τ_friction` is piecewise-linear LUT, 8 break-points, one set per direction. `V_M_measured` is the EMA-filtered ADC1 reading (round-robin pair to ADC0).

**Thermal derate clamp:** when `WARN_R_DRIFT` is set (R drift > 30 % above baseline from opportunistic re-ID — see §7), reduce `DUTY_MAX` proportionally. Mechanism is R-drift based; no separate thermal sensor required.

### 4.7 Backlash crossing sub-profile

1. State → `BRAKE` for 5 ms.
2. State → `COAST` for 5 ms (`nSLEEP = 0`).
3. Flip PHASE.
4. State → `BACKLASH_CROSS`; feedforward inertia-only (`τ_d = J · a_d`). Drive at 30 % of v_max.
5. **Crossing detected** when **(a)** `i_mean > no_load_baseline + 3σ_unloaded` for > 2 ms AND `ripple_freq` drops > 30 % from peak — **OR** **(b)** counts traversed > 1.5 × expected `N_backlash[dir]` (fail-safe).
6. EMA `N_backlash_meas` into `N_backlash[dir]` with α = 0.1.
7. Reset S-curve from current position with `v_init = v_meas`.

### 4.8 End-stop & obstruction detection

Three concurrent detectors in `cwc_inner`; arbitration in `cwc_outer`.

**A — Ripple-stall (fast, ~7 ms):** `ripple_freq < 15 Hz` for ≥ 3 consecutive Goertzel blocks (~7.5 ms) → `EVENT_STALL_CANDIDATE`.

**B — Mean-current threshold (confirmation, ~30–50 ms):** per-direction Welford on `i_mean` during `SETPOINT_RUN` outside backlash zone. `i_mean > welford_mean[dir] + 4σ` for ≥ 5 ms → `EVENT_OVERCURRENT_CANDIDATE`.

**C — Velocity tracking error:** `|v_meas − v_d| > 0.5 · |v_d|` for ≥ 20 ms while `|v_d| > v_creep` → `EVENT_VELOCITY_FAULT`.

**Arbitration:**

| Condition | Interpretation | Action |
|---|---|---|
| A + B AND `\|pos_output − expected_endstop\| < 100` | End-stop reached | BRAKE, latch pos, → `STOPPED_AT_END`, COAST in IDLE |
| A + B AND mid-travel | Obstruction | BRAKE 10 ms, COAST 10 ms, reverse, retract ~200 counts, → IDLE |
| C alone | Slip / external load | log, reduce v_max for rest of traverse |
| B alone | Spurious / supply transient | log, continue |

**Anti-pinch budget:** A (7.5 ms) + B (5 ms) + brake (10 ms) + coast (10 ms) + reverse start (~10 ms) ≈ **42 ms contact-to-retract** vs. ~100 ms FMVSS-118 ceiling. Validate with calibrated force gauge before declaring compliance; do not regress this number during tuning.

**Welford freeze rules:** freeze during backlash crossing, soft-start jerk, decel jerk, end-stop approach. Update only during cruise + steady-state.

**Re-home schedule:** after every `rehome_every_cycles` traverses (host-configurable, default 10), or immediately after any fault. Schedule via `cwc_supervisor` policy.

### 4.9 State machine

```
                 +--------+
   (boot) -----> | INIT   |
                 +---+----+
                     | OP_CWC_STATE_READY received from host (all config replayed)
                     v
                 +--------+
              +->| HOMING |--+
              |  +---+----+  |
              |      | home OK
              |      v
              |  +--------+   directive            +---------+
              +--| IDLE   |---------------------> | MOVING  |
                 | (COAST)|                       +----+----+
                 +---+----+                            |
                     ^                                  | end-stop / target
                     +----------------------------------+
                     |                                  |
                     |  obstruction                     |
                     |  +------------------------------+
                     |  v
                 +---------+
                 | RETRACT |--> IDLE
                 +---------+

   any state, recoverable fault  -> FAULT (clear via OP_CWC_RESET_FAULT)
   any state, fail-stop          -> reboot_with_cause() (infra restarts us)
```

`MOVING` sub-states: `SOFT_START`, `BACKLASH_CROSS`, `CRUISE`, `DECEL`, `CREEP_TO_CONTACT`.

`HALT` and `DEGRADED` are **not** CWC states. Reboot-loop / degraded-mode handling is infra-level (parent project); the CWC class is simply not started when infra is in that state. Hard fail-stops escalate via `reboot_with_cause()`.

---

## 5. External interface

CWC has **no transport opinion**. It produces and consumes `bus_msg_t` envelopes; the slave infra decides whether each one rides RS-485 (to dongle/Pi) or CAN (to vehicle / peripheral). The CWC fills `dst` and `op` in the envelope; manager routes.

When the host (Pi) issues a CWC command:
Pi → dongle (USB-CDC) → libcomm → RS-485 → slave `manager` → `cwc_supervisor` inbox.

When vehicle issues a command via CAN:
Vehicle ECU → CAN → infra `ext_bus_can` → slave `manager` → `cwc_supervisor` inbox.

CWC sees identical `bus_msg_t` from both paths.

---

## 6. Opcodes

All on the libcomm side as `bus_msg_t` opcodes. The class opcode-ID range is allocated by infra at integration; symbolic names used below.

### 6.1 Movement / control

| Opcode | Payload | Source |
|---|---|---|
| `OP_CWC_MOVE_TO` | `int32 target_pos, uint8 flags, uint16 request_id` | host or vehicle |
| `OP_CWC_MOVE_DIR` | `int8 dir, uint8 vscale, uint16 request_id` | host or vehicle |
| `OP_CWC_STOP` | `uint8 mode (0=brake, 1=coast), uint16 request_id` | host or vehicle |
| `OP_CWC_HOME` | `uint8 post_select, uint16 request_id` | host or vehicle |
| `OP_CWC_RESET_FAULT` | `uint16 request_id` | host or vehicle |
| `OP_CWC_ENABLE` | `uint8 enable, uint16 request_id` | host |
| `OP_CWC_CALIBRATE` | `uint8 mode (full/backlash/friction), uint16 request_id` | host |

### 6.2 Configuration (state replay)

Host replays at every boot before `OP_CWC_STATE_READY`. Uses the namespace-based SET opcode against the symbol table:

| Opcode | Payload |
|---|---|
| `OP_CWC_SET_PARAM` | `string name, typed value` — validated against symbol table; only `SYM_WL` symbols accept writes |
| `OP_CWC_STATE_READY` | `none` — signals "all config replayed, you may transition out of INIT" |

Host pushes (typical order):
1. `OP_CWC_SET_PARAM cwc.ident.R 47.5`
2. `OP_CWC_SET_PARAM cwc.ident.Ke 0.0012`
3. ... (rest of ident params, profiles, welford state, backlash, N_poles)
4. `OP_CWC_STATE_READY`

If host has no stored state (first boot for this dongle_id), it sends `OP_CWC_STATE_READY` immediately + `OP_CWC_CALIBRATE` to trigger ident. CWC runs §7 sequence and emits `OP_CWC_STATE_DELTA` events as values are determined. Host captures.

### 6.3 Diagnostic readback

| Opcode | Returns |
|---|---|
| `OP_CWC_GET_SNAPSHOT` | `cwc_snapshot_t` (32 B fixed layout) |
| `OP_CWC_GET_RIPPLE_SPECTRUM` | f_center + 32 log-mag bins + SNR (multi-frame) |
| `OP_CWC_GET_WELFORD` | per-dir mean/sigma/n |
| `OP_CWC_GET_IDENT` | full ident params (multi-frame) |
| `OP_CWC_GET_CYCLE_LOG` | last 32 traverses (multi-frame) |
| `OP_CWC_GET_EVENT_LOG` | last 64 events (multi-frame) |
| `OP_CWC_GET_MANIFEST` | full `cwc.*` symbol table dump (multi-frame) — for host-side validation |

### 6.4 Events (CWC → host)

CWC pushes asynchronously; host accumulates.

| Event | Payload |
|---|---|
| `OP_CWC_EVT_STATE_DELTA` | `string name, typed value` — host updates its state store |
| `OP_CWC_EVT_ENDSTOP_HIT` | post, pos, peak_i_mA, duration_ms |
| `OP_CWC_EVT_OBSTRUCTION` | pos, i_mA, ripple_freq, confidence |
| `OP_CWC_EVT_HOMED` | post, N_backlash_measured |
| `OP_CWC_EVT_AUTOSTOP_REACHED` | target, final, error |
| `OP_CWC_EVT_CALIBRATION_DONE` | success, R, Ke, J |
| `OP_CWC_EVT_FAULT` | fault_flags delta + context |

Every meaningful state change (Welford update, opportunistic R recompute, N_backlash adjustment, cycle count) emits an `OP_CWC_EVT_STATE_DELTA` so the host's stored snapshot is always replayable.

### 6.5 Snapshot payload

```c
typedef struct {
    uint64_t ts_ms;
    uint8_t  state;
    uint8_t  sub_state;
    int8_t   dir;
    uint8_t  reserved;
    int32_t  pos_motor;
    int32_t  pos_output;
    int16_t  v_meas_q8;
    int16_t  v_d_q8;
    uint16_t i_mean_mA;
    uint16_t i_ripple_mA;
    uint16_t ripple_freq_q4;
    uint16_t v_m_mV;
    int16_t  duty_q12;
    uint16_t fault_flags;
} cwc_snapshot_t;               /* 32 B exactly */
```

---

## 7. Parameter identification

Triggered when host sends `OP_CWC_CALIBRATE`, or vehicle sends equivalent CAN cmd, or — during bench bring-up — when the operator runs the `motor.*` shell commands.

Sequence (full-ident path):

1. **Cold-stall R.** Drive at 30 % duty into post A. Steady-state V_M and i_mean → `R = duty · V_M / i_mean`. Should match nominal ~48 Ω at room temp.
2. **Free-spin Ke.** Reverse from post A; during the backlash window (unloaded), 50 % duty 100 ms to settle. `Ke = (duty·V_M − i_mean·R) / ω_meas`, with `ω_meas = 2π · ripple_freq / N_poles`. If N_poles unknown, sweep {3, 5}.
3. **Inertia J.** Apply known torque step (constant i_d) 50 ms; fit `a_meas` from PLL derivative. `J = Kt · i_d / a_meas`.
4. **Friction LUT.** 8 velocities (10–80 % v_max) per direction; steady-state τ = i·Kt − J·a (a ≈ 0).
5. **Backlash widths.** Recorded during each of the four reversal events in the sequence.

Results stream out as `OP_CWC_EVT_STATE_DELTA` events; host captures into its store.

**Opportunistic R re-ID:** during normal operation, recompute R from steady-state cruise (EWMA α = 0.001). `R_recent / R_baseline > 1.30` → `WARN_R_DRIFT` → thermal derate active.

---

## 8. Persistence

**CWC owns none.** The Linux host stores all class state and replays it as opcodes at every boot. The CWC class never writes flash.

What the slave infra writes (CWC has no say):
- `/commission.cbor` — identity (class_id, dongle_id, hardware revision, pin overrides). Written only at commissioning, never during operation.

What the CWC tracks in RAM only (lost on reboot, host re-pushes):
- Ident params, profiles, Welford state, N_backlash, cycle count.
- Cycle log ring (last 32 traverses).
- Event log ring (last 64 events).

This matches the architectural principle: **host is the persistence layer; CWC is the compute layer.** State flows host→CWC at boot (replay) and CWC→host during operation (deltas).

Bench-shell mode breaks this pattern temporarily — see §12. In bench mode, the shell can save config to a CSV via UART; the operator re-loads at next session. Standalone bench operation, not host-managed.

---

## 9. Fault model

`fault_flags` is a uint16. Multiple bits may be set. Reported via `OP_CWC_EVT_FAULT` and visible in `cwc.telemetry.fault_flags`.

| Bit | Name | Severity | Recovery |
|---|---|---|---|
| 0 | `FAULT_OVERCURRENT` | hard | brake, coast, → FAULT, allow `OP_CWC_RESET_FAULT` |
| 1 | `FAULT_ENCODER_STALL` | hard | coast, → FAULT |
| 2 | `FAULT_ENCODER_RIPPLE_MISMATCH` | soft | log, prefer ripple position |
| 3 | `FAULT_RIPPLE_LOST` | soft | encoder-only fallback |
| 4 | `FAULT_VM_OUT_OF_RANGE` | hard | coast, → FAULT |
| 5 | `WARN_R_DRIFT` | warn | reduce v_max via thermal derate |
| 6 | `FAULT_BRIDGE_INFERRED` | hard | duty > 0 + current < expected_min + no motion > 50 ms → FAULT |
| 7 | `FAULT_POSITION_DRIFT` | soft | schedule rehome at next IDLE |
| 8 | `FAULT_SLIP_DETECTED` | soft | reduce v_max, schedule rehome |
| 9 | `FAULT_OBSTRUCTION_REPEATED` | hard | 3 obstructions on same traverse → FAULT |
| 10 | `FAULT_COMMAND_TIMEOUT` | soft | revert to IDLE |
| 11 | `WARN_ENCODER_DEGRADED` | warn | running single-edge fallback |
| 12 | `WARN_BEHAVIOR_BUDGET` | warn | one supervisor tick blew its instruction budget; if persistent, escalates to reboot |
| 13–15 | reserved | | |

`OP_CWC_RESET_FAULT` clears all bits.

### Framework fail-stops (NOT in fault_flags)

Posted via `reboot_with_cause()`; carried in boot-announce on next start.

Standard infra causes: `DEADLINE_MISS`, `QUEUE_OVERFLOW`, `HANDLER_BUDGET`, `BUS_ERROR_BURST`.

CWC-specific user causes (≥ `USER_BASE = 64`):

```c
REBOOT_CAUSE_CWC_BEHAVIOR_STUCK     = 64   // supervisor instruction budget exceeded
REBOOT_CAUSE_CWC_BRIDGE_HARD_FAULT  = 65   // sustained bridge-inferred fault
REBOOT_CAUSE_CWC_VM_HARD_OOR        = 66   // V_M wildly out of range
REBOOT_CAUSE_CWC_IDENT_PERSISTENT_FAIL = 67
```

Each captures up to 12 bytes of context in scratch[1..3].

### Shutdown handler

```c
static void cwc_emergency_shutdown(reboot_cause_t cause, const uint32_t data[3]) {
    pwm_set_enabled(PWM_SLICE_BRIDGE, false);   // stop PWM first
    gpio_put(PIN_NSLEEP, 0);                    // coast (overrides ENABLE)
    gpio_put(PIN_ENABLE,  0);                   // brake (backup)
    gpio_put(PIN_FAULT_LED, 1);                 // visible signal
}
```

Registered with `infra_register_shutdown_handler()` at `cwc_register()`. Called from infra's `reboot_with_cause()` path with IRQs disabled, before scratch write + reset trigger. ISR-safe; no allocation, no FreeRTOS calls, < 1 ms wall time.

**Does not cover hardware watchdog or brown-out resets** — for those, the pull-down resistors on GP10/11/12 (per `hardware_notes.md`) are the only protection.

---

## 10. Supervisor & ChainTree runtime

### 10.1 Runtime location

The ChainTree / s-expr C runtime already exists as plain C; it ports to RP2350 unchanged. Lives in `firmware/pico/common/chaintree_runtime.{h,c}` — slave-infra-shared, reusable by any class.

### 10.2 Behavior is firmware, not state

The CWC behavior program (the ChainTree code that runs in `cwc_supervisor`) is compiled into the firmware via a generated `binary.h`. Build pipeline: source DSL file → runtime's source→binary compiler → C header `const uint8_t cwc_behavior_blob[]`. Included by `cwc_supervisor.c`. **There is no `OP_*_LOAD_BEHAVIOR` opcode; behavior changes require firmware rebuild + reflash.**

### 10.3 Tick procedure

```c
void cwc_supervisor_task(void *arg) {
    runtime_set_step_limit(prog, BEHAVIOR_STEPS_PER_TICK);
    for (;;) {
        wait_for_tick_timer();
        snapshot = copy_telemetry_and_config();  // atomic per-tick view
        steps_used = 0;
        status = runtime_tick(prog, &snapshot, &steps_used);
        if (status == BUDGET_EXCEEDED) {
            reboot_with_cause(REBOOT_CAUSE_CWC_BEHAVIOR_STUCK,
                              last_node_id, steps_used, prog_version);
            /* does not return */
        }
        dispatch_pending_actions();              // forward to outer / manager
    }
}
```

Snapshot semantics: at tick start, supervisor copies current telemetry + config into a local struct; program evaluates against this immutable view. No torn reads, deterministic input bound per tick.

### 10.4 Budget guard — hard fail-stop

Single overrun → reboot via `REBOOT_CAUSE_CWC_BEHAVIOR_STUCK`. No "hold last directive" softening. Consistent with the slave-infra "hard-RT or reboot" rule. The shutdown handler ensures the bridge is disabled before reset.

Step limit set in `cwc_config.h`:

```c
#define BEHAVIOR_STEPS_PER_TICK_10MS   20000
#define BEHAVIOR_STEPS_PER_TICK_100MS  200000
```

At ~5 ns/instruction on M33, 20 000 steps ≈ 100 µs — 1 % of a 10 ms tick. Comfortable headroom under normal operation; triggers well before deadline supervisor would fire.

### 10.5 Symbol namespace

Single registration table drives four consumers: shell, ChainTree runtime, host SET validation, manifest export. Detailed in Appendix A.

Top-level prefixes:
- `sys.*` — infra (uptime, heap, reboot info, CPU idle %, task stats)
- `bus.*` — infra (per-bus counters, debug ring stats)
- `cwc.*` — class-owned

CWC sub-namespaces:
- `cwc.telemetry.*` — read-only fast-updating (snapshotted)
- `cwc.history.*` — read-only per-traverse summary
- `cwc.welford.{up,down}.*` — read-only Welford accumulators
- `cwc.ident.*` — host-write (`SYM_WL`) tunables: R, Ke, Kt, J, friction LUTs, backlash
- `cwc.profile.{up,down}.*` — host-write S-curve parameters
- `cwc.tunable.*` — host-write thresholds (welford k, obstruction_confirm_ms, rehome_every_cycles, antipinch_max_age_ms)
- `cwc.action.*` — invokable actions (move-to, stop, home, reset-fault, etc.)

### 10.6 Access matrix

| Access | Host SET op | Shell `set` | Shell `set` after `unlock` | ChainTree |
|---|---|---|---|---|
| `SYM_RO` | ✗ | ✗ | ✗ | read |
| `SYM_WL` | ✓ | ✗ | ✓ | read |
| `SYM_RW` | ✗ | ✓ | ✓ | read |
| `SYM_ACT` | ✗ | invoke | invoke | invoke as special-form |

### 10.7 ChainTree action special-forms

```
(move-to pos [flags])           → cwc.action.move-to
(move-dir dir vscale)           → cwc.action.move-dir
(stop kind)                     → cwc.action.stop
(home post)                     → cwc.action.home
(reset-fault)                   → cwc.action.reset-fault
(set-mode mode)                 → cwc.action.set-mode
(start-ident kind)              → cwc.action.start-ident
(emit-event code payload)       → cwc.action.emit-event
(post-warn flag)                → cwc.action.post-warn (transient fault_flags bit)
(reboot cause d0 d1 d2)         → cwc.action.reboot (CWC user causes)
```

Each resolves to a `SYM_ACT` entry in the symbol table; both shell commands and ChainTree special-forms invoke through the same dispatch path.

---

## 11. Debug ring + UART1 drainer

### 11.1 Ring buffer

Single SRAM ring as the sole output channel from CWC to UART1. Any thread can append records; one drainer task pumps the ring to UART via DMA. **No thread writes UART1 directly.**

```c
#define CWC_DBG_RING_SIZE_BYTES   32768
#define CWC_DBG_RECORD_SIZE       16
#define CWC_DBG_RECORD_COUNT      (CWC_DBG_RING_SIZE_BYTES / CWC_DBG_RECORD_SIZE)

typedef struct {
    uint8_t  type;          // record type — 0xA5 sync-byte marker on first byte for resync
    uint8_t  producer_id;
    uint16_t ts_low_ms;
    uint8_t  payload[12];
} dbg_record_t;             /* 16 B exactly */
```

Fixed-size makes lock-free MPMC trivial: atomic `fetch_add` on write index reserves a slot, plain stores fill it, atomic store on a "released" flag in the slot header marks it ready. Drainer skips not-yet-released slots.

### 11.2 Producers

| Producer | What it appends |
|---|---|
| `cwc_inner` | decimated sample records (default 1 per 32-sample block = ~780 Hz) |
| `cwc_outer` | state transitions, traverse summaries |
| `cwc_supervisor` | tick decisions, mode changes |
| `cwc_emergency_shutdown` | pre-reboot snapshot (ISR-safe, single record) |
| fault dispatch | fault events |
| shell command handlers | command responses (when shell mode is implemented) |

**Discipline:** producers never block. Ring full → record dropped, `bus.dbg.dropped` counter increments. Telemetry loss is acceptable; control loss is not — this asymmetry is deliberate.

### 11.3 Drainer task

`cwc_dbg_drainer` (LOW prio, core 0, event-driven):

```c
void cwc_dbg_drainer_task(void *arg) {
    for (;;) {
        size_t n = ring_peek_contiguous(&dbg_ring);
        if (n == 0) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        dma_transfer_blocking(UART1_TX_DMA, ring_read_ptr(&dbg_ring), n);
        ring_advance_read(&dbg_ring, n);
    }
}
```

UART1 at 1 Mbaud → 100 KB/s drain rate. Producers notify drainer after appending.

### 11.4 Bandwidth budget

| Stream | Per-record | Rate | KB/s | Fits |
|---|---|---|---|---|
| Outer state events | 16 B | irregular | ~5 | yes |
| Inner decimated (1 per block) | 16 B | 780 Hz | 12 | yes |
| Full inner (every sample) | 16 B | 25 kHz | **400** | **no — line limit ~100** |
| Decimated inner (1 per 4 blocks) | 16 B | 195 Hz | 3 | yes |

For full-rate captures, use trigger-and-dump: ring fills in ~80 ms at 400 KB/s, drainer pumps for ~320 ms while inner reverts to decimated. Single-shot scope-like capture.

---

## 12. Bench shell architecture

### 12.1 Two modes

| Mode | When | Hardware owner | Shell role |
|---|---|---|---|
| **Bench** | now → infra lands | shell + minimal ISRs | shell IS the OS; direct GPIO/PWM/ADC; no libcomm, no production tasks |
| **Production** | post-parent-P5 | `cwc_outer/inner/supervisor` | shell is a debug overlay; commands invoke symbol-table actions; raw HW writes require `unlock` |

Same source tree; two `main()` entry points selected at build time.

### 12.2 Two-layer shell

- **General-purpose shell core** (`firmware/pico/common/dbg_shell.{c,h}` — infra-owned, reusable). Provides line parser, command-registration framework, generic commands (`help`, `list`, `tree`, `get`, `set`, `tasks`, `heap`, `queues`, `reboot`, `unlock`, `lock`, `estop`, `version`).
- **CWC application shell** (`firmware/pico/dongle/cwc/src/shell_cwc.c`). Registers CWC-specific commands at startup.

### 12.3 Source layout

```
firmware/pico/dongle/cwc/
├── CMakeLists.txt                # selects bench_main.c or cwc_main.c via target
├── continue.md                   # this file
├── hardware_notes.md             # pull-down + bench verification
├── include/
│   ├── cwc_config.h
│   ├── cwc_packets.h
│   └── cwc_symbols.h
└── src/
    ├── bench_main.c              # bench-mode entry: shell-as-OS, no libcomm
    ├── cwc_main.c                # production entry: cwc_register() against real infra
    ├── cwc_symbols.c             # the registration table (canonical surface)
    ├── shell_hw.c                # hw.* / adc.* / encoder.* commands
    ├── shell_motor.c             # motor.* characterization recipes
    ├── shell_ctrl.c              # ctrl.* control-law commands
    ├── shell_capture.c           # capture.* / trace.*
    ├── cwc_outer.c               # production: state machine + S-curve
    ├── cwc_inner.c               # production: 25 kHz inner loop
    ├── cwc_supervisor.c          # production: ChainTree tick
    ├── cwc_emergency_shutdown.c  # shared shutdown hook
    ├── bridge.c                  # shared HW driver
    ├── adc_dma.c                 # shared HW driver
    ├── encoder.c                 # shared HW driver
    ├── ripple.c                  # shared DSP
    ├── welford.c                 # shared DSP
    ├── scurve.c                  # shared planner
    ├── feedforward.c             # shared control
    └── cwc_behavior_binary.h     # GENERATED from behavior source by build step
```

Both `bench_main.c` and `cwc_main.c` link against the same lower-level drivers. Only the active task tree and the command registration differ.

### 12.4 Bench shell command surface

**Generic infra-shared (no class prefix):**

```
help [pattern]
list [pattern]             walk namespace
tree [prefix]              tree-format dump
get <name>                 read symbol
set <name> <value>         write symbol (access-controlled)
unlock                     enable SYM_RW + raw hw.* writes
lock                       re-lock
estop                      immediate coast (any mode, no unlock needed)
tasks                      FreeRTOS task table
heap
queues
uptime
version
cause                      prev_reboot_cause + scratch
reboot [cause]
```

**Raw hardware (bench mode, locked behind `unlock`):**

```
hw.gpio set <pin> 0|1
hw.gpio read <pin>
hw.pwm set <duty>          0..1 on GP11
hw.bridge phase <0|1>
hw.bridge sleep <0|1>
hw.bridge enable <0|1>     bypasses PWM
hw.led <on|off|pattern>
```

**Sensor primitives:**

```
adc.read <ch>
adc.stream <rate>          round-robin both channels → debug ring
adc.zero-cal               bridge disabled, measure INA240 offset
encoder.reset
encoder.read
encoder.stream <rate>
```

**Motor characterization (each one ≈ one §7 ident step, decomposed):**

```
motor.r-measure <post>            drive into post, sweep duty, compute R
motor.ke-measure                  reverse from post, capture backlash window, compute Ke
motor.j-measure                   step current, fit a_meas, compute J
motor.friction-curve <dir> <n>    n steady-state velocities, compute LUT
motor.backlash <dir>              count edges to engagement
motor.spectrum <duty> <duration>  capture for offline FFT
```

Each emits a labeled CSV block into the debug ring; host saves to `motor_chars/<date>_<test>.csv`.

**Control-law iteration:**

```
config.set <symbol> <value>          write any cwc.ident.* / cwc.profile.* / cwc.tunable.*
config.show [prefix]
config.save <file>                   serialize current config as CSV; host catches via UART
config.load <file>                   host replays previously saved config
ctrl.duty <pct> <duration_ms>        fixed-duty move
ctrl.move <pos>                      open-loop scurve to target using current config
ctrl.scurve-test <pos> <profile>     scurve with explicit profile values
ctrl.ff-test <v_d> <a_d>             apply feedforward eq, log
```

**Telemetry capture:**

```
capture.arm <duration_ms> <trigger>   trigger ∈ {now | obstruction | endstop | shell-cmd}
capture.dump
trace.start <symbol-list> <rate>
trace.stop
```

### 12.5 Pre-infra bring-up flow

Bench mode produces:

1. **Validated hardware** — every GPIO, every pull-down, every ADC channel exercised per `hardware_notes.md` § verification.
2. **Characterized motor** — R, Ke, Kt, J, friction LUT, backlash all measured. CSV files become the host's replay store once production mode lands.
3. **Validated control law** — S-curve + feedforward proven at known parameters.
4. **A regression script** — `bench_tests.sh` (or LuaJIT) runs all `motor.*` commands, compares output CSVs against golden references. Catches drift after any firmware change.

Bench shell **freezes** when production mode lands — it stays as a debug tool but doesn't grow new features. New functionality goes through production paths (libcomm opcodes, ChainTree).

### 12.6 Build-time toggles — `#if 1` / `#if 0` pattern

Both the debug UART layer (§11) and the shell parser layer (§12) follow the slave-infra convention: a literal `#if 1 ... #endif` block at the top of each source file gates the whole layer in or out. No CMake option, no `-D` flag.

Files:
- `firmware/pico/common/dbg_uart.c` — UART + ring + drainer.
- `firmware/pico/common/dbg_shell.c` — generic shell parser + infra commands.
- `firmware/pico/dongle/cwc/src/shell_cwc.c` — CWC-specific shell command registrations.

To toggle, edit the source from `#if 1` to `#if 0` (or back). The toggle state lives in the file it controls and is committed alongside the code.

Default in dev: all three `#if 1`.

Production variants:
- **Telemetry-only** (`dbg_uart.c #if 1`, both shell files `#if 0`): debug ring still drains over UART, no interactive surface. Useful for fielded slaves with a logger on the harness.
- **Fully stripped** (all three `#if 0`): UART pins (GP4/GP5) freed for other use; ~10 KB code + 32 KB SRAM recovered. Use when GPIO budget or security posture forbids the debug surface.

Headers in `firmware/pico/common/include/dbg_*.h` gate their declarations to match the source `#if` state so the linker is consistent. The four consumers of the symbol-registration table (host SET, ChainTree runtime, shell, manifest export) all keep working with the shell stripped — they share the underlying registration, not the shell parser.

Branch discipline replaces build-flag discipline: production branches commit `#if 0` for the layers they don't want; CI builds the branch as-committed. A small `tools/test_build_variants.sh` flips the toggles and rebuilds nightly so neither variant rots.

---

## 13. Build & bring-up

### 13.1 Toolchain

pico-sdk 2.x, FreeRTOS-Kernel (RPi fork) SMP, arm-none-eabi-gcc, CMake. Per `pico_sdk_freertos_setup.md` recipe: `configSUPPORT_STATIC_ALLOCATION=1`, `configKERNEL_PROVIDED_STATIC_MEMORY=1`, `configGENERATE_RUN_TIME_STATS=1`.

### 13.2 Bring-up order (assuming bench mode first, production mode after infra P5)

**Phase A — bench mode, no infra:**

| Step | Goal | Pass criterion |
|---|---|---|
| A0 | Skeleton: shell parser running, UART1 echoes, debug ring drains | `help` produces output |
| A1 | GPIO + pull-down verification per `hardware_notes.md` | All bridge pins < 0.5 V across reset paths |
| A2 | `hw.pwm set 0.3` → motor moves both directions | Visible motion, current matches Ohm's law |
| A3 | `adc.stream` + `adc.zero-cal` | Currents match multimeter; V_M matches scope |
| A4 | `encoder.stream` while jogging | Counts track shaft rotation, direction correct |
| A5 | Ripple HPF + Goertzel + PLL via `motor.spectrum` capture | `ripple_freq` tracks duty-driven RPM linearly |
| A6 | Direction-segregated Welford via `adc.stream` during sweep | Mean/sigma stable per direction |
| A7 | S-curve + feedforward via `ctrl.scurve-test` | Jerk-limited, no thunk, zero-load smooth |
| A8 | Homing recipe (`motor.r-measure` into both posts) | Repeatable, `N_backlash_*` populated |
| A9 | Anti-pinch test with stick obstruction | Detect-to-retract ≤ 50 ms (scope the brake transition) |
| A10 | Full `motor.*` characterization recipe | Complete CSV captured; values within tolerance of spec |
| A11 | ChainTree behavior loaded via `binary.h` rebuild, supervisor ticks at 100 Hz | No spurious reboots; budget guard not triggered |

**Phase B — production mode, against real infra (post-P5):**

| Step | Goal | Pass criterion |
|---|---|---|
| B0 | `cwc_register()` against real `manager`/`internal_bus` | Tasks registered with deadline supervisor; health snapshot shows them |
| B1 | `OP_CWC_STATE_READY` round-trip from host, with config replay | Phase-A characterization values load cleanly, transition INIT → HOMING |
| B2 | Motion command from host via libcomm round-trips through dongle | `OP_CWC_MOVE_TO` produces motion identical to A7 bench result |
| B3 | Vehicle CAN cmd via infra `ext_bus_can` | Same motion as B2 from vehicle path |
| B4 | Fault injection per fault_flags bit | Each bit set produces expected event + recovery |
| B5 | Reboot loop intentionally provoked (force `BEHAVIOR_STUCK`) | Infra detects, enters degraded mode, host sees diagnostic |

### 13.3 Constraints

- **No dynamic allocation** on inner / outer / supervisor paths.
- **No flash writes** from CWC code (infra owns the commission record).
- **IRQ affinity:** all CWC IRQs (ADC DMA-done, encoder PIO IRQ, UART1 TX done) routed to **core 1** for inner-loop adjacency, except UART1 TX-done which lives on core 0 with the drainer.
- **No CWC-side watchdog feeding** — infra's guardian owns that.

---

## 14. Open questions

1. **PIO + GP coordination with slave infra.** §2 assumes infra has not claimed GP4-GP15, GP22, GP26-GP28. Confirm at integration which PIO instance(s) and SMs are infra-reserved.
2. **N_poles** of specific N20 — confirm with scope on first hardware. Spec assumes 3; ident sweeps {3, 5}.
3. **CAN-FD vs classic CAN** — classic for v1. Vehicle CAN ID conventions arrive at integration with the eventual vehicle CAN side.
4. **Bridge-fault inference thresholds** — 50 ms window, "expected_min" current, "no motion" criterion all need HIL tuning to avoid false positives at low duty.
5. **Anti-pinch force calibration** — validate ~42 ms detect-to-retract budget with calibrated force gauge before declaring FMVSS-118 style compliance.
6. **CWC opcode ID allocation** — concrete IDs come from infra opcode allocation table; CWC uses symbolic names until then.
7. **ChainTree runtime symbol-binding API shape** — confirm runtime exposes `bind_symbol(name, type, storage)` and `register_special_form(name, callback)` registration calls.
8. **Compile-time symbol resolution** — does the runtime's source→binary compiler resolve `(get cwc.telemetry.pos_output)` to a numeric index at build time? If yes, supervisor's hot tick is O(1) lookup; if no, runtime cost depends on cache hit rate.
9. **Behavior step-limit hook** — does the C runtime already have per-tick instruction counting, or does it need to be added? One-line modification in the eval inner loop if not.
10. **Infra shutdown-handler registration API** — to be added alongside `reboot_cause.{c,h}`. Universal across slave classes, not CWC-specific.
11. **Infra reboot-loop / degraded-mode mechanism** — also infra-side, parallel to commissioning state. CWC class simply isn't started in that state.

---

## Appendix A — Symbol namespace (cwc.*)

Full canonical table; mirrored in `src/cwc_symbols.c` registration array.

```
cwc.telemetry.pos_motor              int32   counts
cwc.telemetry.pos_output             int32   counts
cwc.telemetry.v_meas                 float   counts/sec
cwc.telemetry.a_meas                 float   counts/sec²
cwc.telemetry.i_mean                 float   A
cwc.telemetry.i_ripple_amp           float   A
cwc.telemetry.ripple_freq            float   Hz
cwc.telemetry.ripple_count           int32   counts
cwc.telemetry.v_m_measured           float   V
cwc.telemetry.duty                   float   -1..+1
cwc.telemetry.state                  enum    INIT/HOMING/IDLE/MOVING/RETRACT/FAULT
cwc.telemetry.sub_state              enum    SOFT_START/BACKLASH_CROSS/CRUISE/DECEL/CREEP_TO_CONTACT
cwc.telemetry.dir                    int8
cwc.telemetry.fault_flags            uint16
cwc.telemetry.mode                   enum    MANUAL/AUTO_UP/AUTO_DOWN
cwc.telemetry.tick_id                uint32
cwc.telemetry.link_alive             bool

cwc.history.last_traverse_duration_ms  uint32
cwc.history.last_end_reason            enum   TARGET/ENDSTOP/OBSTRUCTION/FAULT
cwc.history.last_peak_i_mA             uint16
cwc.history.traverse_count             uint32

cwc.welford.up.mean                  float   A
cwc.welford.up.sigma                 float   A
cwc.welford.up.n                     uint32
cwc.welford.down.mean                float   A
cwc.welford.down.sigma               float   A
cwc.welford.down.n                   uint32

cwc.ident.R                          float   Ω           (SYM_WL)
cwc.ident.Ke                         float   V·s/rad     (SYM_WL)
cwc.ident.Kt                         float   N·m/A       (SYM_WL)
cwc.ident.J                          float   kg·m²       (SYM_WL)
cwc.ident.N_backlash_up              int16               (SYM_WL)
cwc.ident.N_backlash_down            int16               (SYM_WL)
cwc.ident.friction_lut_up            float[8]            (SYM_WL)
cwc.ident.friction_lut_down          float[8]            (SYM_WL)
cwc.ident.N_poles                    uint8               (SYM_WL)
cwc.ident.ident_ts_ms                uint64

cwc.profile.up.v_max                 float   counts/sec  (SYM_WL)
cwc.profile.up.a_max                 float               (SYM_WL)
cwc.profile.up.j_max                 float               (SYM_WL)
cwc.profile.up.v_creep               float               (SYM_WL)
cwc.profile.up.decel_margin          int32               (SYM_WL)
cwc.profile.down.{same five}

cwc.tunable.welford_k_threshold      float               (SYM_WL)
cwc.tunable.obstruction_confirm_ms   uint16              (SYM_WL)
cwc.tunable.rehome_every_cycles      uint16              (SYM_WL)
cwc.tunable.antipinch_max_age_ms     uint16              (SYM_WL)

cwc.action.move-to                   action  (int32 pos)
cwc.action.move-dir                  action  (enum dir, uint8 vscale)
cwc.action.stop                      action  (enum mode)
cwc.action.home                      action  (enum post)
cwc.action.reset-fault               action
cwc.action.set-mode                  action  (enum mode)
cwc.action.start-ident               action  (enum kind)
cwc.action.emit-event                action  (uint16 code, blob payload)
cwc.action.post-warn                 action  (uint16 flag)
cwc.action.reboot                    action  (uint8 cause, uint32×3 data)
```

Plus the infra-provided `sys.*` and `bus.*` namespaces (defined in slave-infra spec).

---

## Appendix B — Glossary

- **Slave infra** — the libcomm/FreeRTOS-SMP framework on the slave Pico: bus_kernel_pico, manager + internal_bus, ext_bus_pio_rs485 + ext_bus_can, deadline supervisor, LittleFS for commission record, reboot-loop detection. Lives in `firmware/pico/common/` and the slave skeleton.
- **Class** — a libcomm logical_robot implementation. Here: CWC, instantiated as four tasks (`cwc_outer` + `cwc_inner` + `cwc_supervisor` + `cwc_dbg_drainer`).
- **bus_msg_t** — libcomm's 40-byte wire envelope (8 B header + 32 B inline payload). CWC produces/consumes these; framing/CRC/transport handled by infra.
- **Bench mode** — pre-infra build of the firmware where shell is the OS; used for hardware bring-up and motor characterization.
- **Production mode** — post-infra build where `cwc_register()` plugs into the real infra; shell is a debug overlay.
- **ChainTree runtime** — pre-existing C-language runtime for the s-expression / behavior-tree DSL. Ported unchanged to RP2350. Lives in `firmware/pico/common/chaintree_runtime.{c,h}`.
- **Shutdown handler** — application-registered ISR-safe callback invoked by `reboot_with_cause()` before any software-triggered reset, to put hardware in a safe state.
- **Debug ring** — 32 KB SRAM ring buffer; producers append records lock-free; `cwc_dbg_drainer` pumps to UART1 at 1 Mbaud. Drop-on-full (telemetry loss acceptable).
- **State replay** — pattern where the Linux host stores all CWC config/state and re-pushes it as opcodes at every boot. CWC owns no persistent state.

End of continue.md
