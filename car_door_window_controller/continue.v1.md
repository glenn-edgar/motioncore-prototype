# continue.md — Car Window Controller (CWC)

**Author:** Glenn Edgar / Onyx Engineering
**Target:** Raspberry Pi Pico 2 (RP2350, dual Cortex-M33 + DSP + FPU)
**Status:** Architectural spec — ready for Claude Code implementation handoff.
**Philosophy:** fail-fast, exception-based, incremental. ChainTree-compatible packet model.

---

## 0. Scope & handoff

Build the firmware for a car-door-window simulator: brushed N20 worm-drive motor, 270° travel between foam end posts, high-side current sensing, encoder + commutation-ripple feedback, S-curve trajectory planning with open-loop torque feedforward, anti-pinch obstruction detection, CAN-bus vehicle interface, and RS485 ChainTree diagnostic/control bus.

The deliverable from Claude Code is a buildable Pico 2 firmware project (C or C++ via pico-sdk, or Rust via embassy/rp-hal — implementer's choice; prefer C with pico-sdk for tightest peripheral control unless reason to deviate). All algorithms below are specified with concrete numerics. Where a value is `TBD` it must be either left as a `#define`/const exposed to runtime configuration, or determined during the parameter-ID cycle (see §7).

---

## 1. Hardware BOM

| Block | Part | Notes |
|---|---|---|
| MCU | RP2350 (Pico 2) | dual M33, FPU, DSP ext, 3× PIO, 264 KB SRAM |
| H-bridge | TI DRV8838 | PHASE/ENABLE, slow-decay, 1.8–11 V, 1.8 A |
| Current sense | TI INA240A4 | gain 200 V/V, V_S = 3.3 V, REF tied to GND, unidirectional |
| Shunt | 100 mΩ, ≥0.5 W, 1% | high-side, between V_M supply and DRV8838 VM pin |
| Motor | N20 brushed DC, 238:1 gearbox | 7 pulses/motor-rev encoder, 3-pole armature (verify with scope) |
| V_M supply | 6 V regulated | I_stall ≈ 125 mA → I²R losses minimal |
| CAN PHY | MCP2517FD + ATA6563 (or equivalent) | SPI to RP2350, CAN-FD capable, classic CAN mode default |
| RS485 PHY | SN65HVD78 (or MAX3485) | half-duplex, DE/RE tied together to GPIO |
| Misc | status LED, fault LED, V_M divider to ADC | thermal vias under DRV8838 pad |

End posts: foam, deformable, ~10–20 N compression force. They are *both* the mechanical home reference and the over-travel limit.

---

## 2. Pin map (Pico 2)

| GP# | Function | Notes |
|---|---|---|
| GP0 | UART0 TX → RS485 driver | |
| GP1 | UART0 RX ← RS485 receiver | |
| GP2 | RS485 DE/RE | high = transmit |
| GP4 | SPI0 SCK → MCP2517FD | |
| GP5 | SPI0 MOSI → MCP2517FD | |
| GP6 | SPI0 MISO ← MCP2517FD | |
| GP7 | MCP2517FD CS | |
| GP8 | MCP2517FD INT | falling-edge IRQ |
| GP10 | DRV8838 PHASE | direction |
| GP11 | DRV8838 ENABLE | PWM out, PWM slice |
| GP12 | DRV8838 nSLEEP | low = coast/sleep |
| GP14 | Encoder A | PIO1 SM0 input |
| GP15 | Encoder B | PIO1 SM0 input (quadrature if available) |
| GP25 | Onboard LED — status | |
| GP22 | Fault LED | |
| GP26 / ADC0 | INA240 V_OUT | current sense |
| GP27 / ADC1 | V_M divider | supply-voltage feedforward |
| GP28 / ADC2 | spare | temperature sensor optional |

PIO allocation:
- **PIO0** — PWM trigger generator + ADC sample trigger (sync ADC to mid-ON of PWM cycle).
- **PIO1** — quadrature encoder counter (32-bit signed).
- **PIO2** — reserved (spare; candidate for software CAN via `can2040` if MCP is dropped, or for second UART).

---

## 3. Architecture

### 3.1 Compute split

| Core | Rate | Responsibilities |
|---|---|---|
| **Core 0** | 1 kHz outer + event-driven | State machine, S-curve trajectory generator, bus I/O (CAN+RS485), parameter ID coordinator, persistence, fault dispatch |
| **Core 1** | 25 kHz inner | ADC ingest, Goertzel + PLL ripple analysis, Welford stats, open-loop torque feedforward, PWM duty update, end-stop/obstruction detector primitives |

### 3.2 Inter-core interface

Single-producer single-consumer ring buffer in SRAM, lock-free via RP2350 SIO FIFO + a shared `setpoint_t` struct double-buffered with sequence number.

```c
typedef struct {
    uint32_t seq;          // monotonic, increment on each update
    int32_t  p_d;          // desired position, encoder counts (signed)
    float    v_d;          // desired velocity, counts/sec
    float    a_d;          // desired acceleration, counts/sec²
    uint8_t  mode;         // SETPOINT_IDLE, _RUN, _BACKLASH_CROSS, _CREEP, _BRAKE, _COAST
    int8_t   dir;          // -1, 0, +1
} setpoint_t;
```

Core 1 publishes telemetry the other direction via a second ring:

```c
typedef struct {
    uint32_t seq;
    int32_t  pos_motor;    // raw encoder count
    int32_t  pos_output;   // backlash-compensated
    float    v_meas;       // from ripple PLL, counts/sec
    float    i_mean;       // EMA mean current, A
    float    i_ripple_amp; // Goertzel magnitude
    float    ripple_freq;  // Hz
    float    welford_mean[2];  // per-direction
    float    welford_sigma[2];
    uint16_t fault_flags;
} inner_telemetry_t;
```

### 3.3 Data flow

```
ADC -> DMA ring -> Core1 (25 kHz block) -> Goertzel/PLL -> Welford -> FF map -> PWM duty
                                                |
                                                v
                                          inner_telemetry_t -> Core0
                                                                 |
Core0: state machine + S-curve gen -> setpoint_t -> Core1
       |
       +--> CAN TX (50 Hz status, event-driven for ACK/fault)
       +--> RS485 TX (diagnostic, on poll or scheduled)
       +--> Flash persistence (on STOPPED transitions)
```

---

## 4. Subsystems

### 4.1 Bridge driver (DRV8838)

Truth table used:

| nSLEEP | ENABLE | PHASE | Result |
|---|---|---|---|
| 0 | x | x | Coast (Hi-Z) |
| 1 | 0 | x | Brake (low-side short) |
| 1 | PWM | 0 | Drive reverse, slow-decay |
| 1 | PWM | 1 | Drive forward, slow-decay |

- **PWM** on ENABLE, 25 kHz, 12-bit duty resolution.
- **PHASE** held during PWM cycles; change only at zero-velocity transitions.
- **Direction reversal protocol**: brake (5 ms) → coast (`nSLEEP=0`, 5 ms) → flip PHASE → re-enter drive via backlash sub-profile. Never flip PHASE under PWM — current transient risks bridge fault.

### 4.2 Current sensing (INA240A4)

- Shunt 100 mΩ, gain 200 V/V → 20 V/A scaling.
- V_S = 3.3 V, REF1 = REF2 = GND (unidirectional, high-side only sources positive current).
- Stall at 6 V ≈ 125 mA → V_OUT = 2.5 V → fits 3.3 V ADC range with 25% headroom.
- ADC: 12-bit, 805 µV/LSB → 40 µA/LSB current resolution.
- **Sample timing**: PIO0 triggers ADC at 70% of PWM ON window. During OFF (slow-decay brake), current bypasses the high-side shunt; do not sample.
- **DMA**: ADC → SRAM ring buffer (128 samples deep). Core 1 consumes in blocks of 32 (1.28 ms latency).

Discard samples flagged as overrange. Persistent overrange (≥ 3 in any 32-sample block) → `FAULT_OVERCURRENT`.

### 4.3 Encoder

- N20 magnetic encoder: 7 pulses per motor rev, quadrature if both A and B wired.
- After gearbox: 7 × 238 = 1666 single-edge counts per output rev. 270° travel ≈ 1250 counts.
- PIO1 SM0: standard quadrature decoder, 32-bit signed accumulator readable via PIO RX FIFO + DMA to a shared atomic uint32.
- **Encoder stall watchdog**: if `|Δcount|` over 100 ms is < 2 while `|duty| > 10%`, raise `FAULT_ENCODER_STALL` (distinct from intentional stop or end-stop).

### 4.4 Ripple analysis

Brushed-DC commutation ripple = N_poles ripples per motor revolution. Assume 3-pole until verified. Ripple frequency band: ~0 Hz (stalled) to ~500 Hz (worst-case running). Bandwidth requirement at Goertzel: 25 kHz sample rate, Nyquist 12.5 kHz, comfortable.

**Pipeline (Core 1, 25 kHz):**

1. **High-pass IIR** on raw current samples: 1st-order, fc ≈ 20 Hz, removes DC + load drift. Output = ripple-only signal `r[n]`.
2. **Goertzel** at tracking frequency f₀ (initially seeded at 100 Hz, updated by PLL). Block size 64 samples (2.56 ms). Outputs magnitude `R_mag` and phase `R_phase`.
3. **PLL**: phase-detector = R_phase derivative; loop filter PI; VCO output = current f₀ estimate. Bound f₀ to [10 Hz, 800 Hz]. Slew limited to ±200 Hz/s.
4. **Ripple counter**: zero-cross detector on r[n], debounced by ¼-cycle holdoff based on f₀. Increment `ripple_count` signed by `dir`.

**Outputs to Core 0:**
- `ripple_freq` (Hz) — instantaneous motor speed × N_poles.
- `ripple_amp` (A) — Goertzel magnitude.
- `ripple_count` — incremental, used as redundancy for encoder.

**Cross-check rule:** expected ratio is `ripple_per_encoder = N_poles / 7` (e.g., 3/7 ≈ 0.4286). If 5-sample moving ratio deviates > 25% from expected for > 500 ms during active drive, raise `FAULT_ENCODER_RIPPLE_MISMATCH`. Decide which is correct by majority vote with prior known-good cycle.

### 4.5 S-curve trajectory generator (Core 0, 1 kHz)

Jerk-limited 7-segment profile. Inputs: `p_start`, `p_target`, `v_max`, `a_max`, `j_max`. Generates `(p_d(t), v_d(t), a_d(t))`.

Stored profile parameters per direction (gravity-loaded vs gravity-helped have different friction → different optimal v_max):

```c
typedef struct {
    float v_max;       // counts/sec
    float a_max;       // counts/sec²
    float j_max;       // counts/sec³
    float v_creep;     // counts/sec, final approach to end-stop
    int32_t decel_margin;  // counts before expected endstop where decel completes
} traj_profile_t;

traj_profile_t profile_up, profile_down;
```

**Defaults** (tune during ID):
- `v_max` ≈ 800 counts/sec (full 270° in ~1.5 s)
- `a_max` ≈ 4000 counts/sec²
- `j_max` ≈ 40000 counts/sec³
- `v_creep` ≈ 40 counts/sec (5% of v_max)
- `decel_margin` ≈ 10 counts

Algorithm: standard scurve_3 / ruckig-style, or in-house. Reentrant; supports retargeting mid-profile (for obstruction reverse).

### 4.6 Open-loop torque feedforward (Core 1, 25 kHz)

Per PWM cycle, after ADC sample and stats update:

```
τ_d        = J * a_d + τ_friction(v_d, dir)
i_d        = τ_d / Kt
V_cmd      = R * i_d + Ke * v_d
duty_raw   = V_cmd / V_M_measured
duty       = clamp(duty_raw, -DUTY_MAX, +DUTY_MAX)

if (duty >= 0) { PHASE = dir_to_phase(commanded_dir); }
PWM = abs(duty)
```

Coefficients sourced from `ident_params_t` (see §7), loaded from flash on boot. `τ_friction` is a piecewise linear LUT indexed by `|v_d|` with 8 breakpoints, one set per direction.

`V_M_measured` is the current ADC reading on the V_M divider, low-passed (EMA, τ ≈ 50 ms) to reject ripple but track battery sag.

### 4.7 Backlash crossing sub-profile

On direction reversal command:

1. State → `BRAKE` for 5 ms (ENABLE = 0).
2. State → `COAST` for 5 ms (nSLEEP = 0).
3. Flip PHASE.
4. State → `BACKLASH_CROSS`: set `mode = SETPOINT_BACKLASH_CROSS`. Feedforward switches to inertia-only model (`τ_d = J·a_d`, no friction term — there's no load). Drive at constant 30% of `v_max` setpoint.
5. Crossing detected when **both**:
   - `i_mean` rises above `(no_load_baseline + 3σ_unloaded)` for > 2 ms, AND
   - `ripple_freq` drops by > 30% from peak observed during crossing
6. At detection: latch `N_backlash_meas` = encoder counts traversed during this phase. EMA into `N_backlash[dir]` with α = 0.1 (slow adapt).
7. Reset S-curve from current position with `v_init = v_meas`, hand off to main trajectory.

`N_backlash[dir]` is used by `pos_output` calculation: encoder counts within the backlash window after a reversal do not advance `pos_output`.

### 4.8 End-stop & obstruction detection

Three concurrent detectors run on Core 1 with Core 0 arbitration.

**Detector A — Ripple-stall (fast path, ~5–10 ms):**
- Trigger: `ripple_freq < 15 Hz` for ≥ 3 consecutive Goertzel blocks (≈ 7.5 ms).
- Action: raise `EVENT_STALL_CANDIDATE` to Core 0; Core 0 cross-checks position.

**Detector B — Mean-current threshold (confirmation, ~30–50 ms):**
- Per-direction Welford on `i_mean` running only when `mode == SETPOINT_RUN` and outside backlash zone.
- Trigger: `i_mean > welford_mean[dir] + 4·welford_sigma[dir]` for ≥ 5 ms.
- Action: raise `EVENT_OVERCURRENT_CANDIDATE`.

**Detector C — Velocity tracking error:**
- `|v_meas - v_d| > 0.5 * |v_d|` for ≥ 20 ms while `|v_d| > v_creep`.
- Action: raise `EVENT_VELOCITY_FAULT`.

**Core 0 arbitration:**

| Condition | Interpretation | Action |
|---|---|---|
| Detector A + B fire AND `\|pos_output - expected_endstop\| < 30` | End-stop reached | `BRAKE`, latch pos, → `STOPPED_AT_END` |
| Detector A + B fire AND mid-travel | Obstruction | `BRAKE` 10 ms, `COAST` 10 ms, reverse direction, run sub-profile to retract ~50 counts, → `IDLE` after reversal |
| Detector C alone | Slip or external load | log warning, reduce v_max for remainder of traverse |
| Detector B alone, no A | Spurious or supply transient | log, continue |

Welford reset/freeze rules:
- Freeze (do not update) during: backlash crossing, soft-start jerk phase, decel jerk phase, end-stop approach.
- Update only during cruise + steady-state phases.
- Persist across power cycles (per direction).

### 4.9 State machine

```
                 +--------+
   (boot) -----> | INIT   |
                 +---+----+
                     | params loaded
                     v
                 +--------+
              +->| HOMING |--+
              |  +---+----+  |
              |      | home OK
              |      v
              |  +--------+   cmd_move      +---------+
              +--| IDLE   |---------------> | MOVING  |
                 +---+----+                 +----+----+
                     ^                           |
                     |     end-stop / target     |
                     +---------------------------+
                     |                           |
                     |     obstruction           |
                     |   +---------------------- +
                     |   v
                 +--------+
                 | RETRACT|--> IDLE
                 +--------+

   any state, fault -> FAULT (recoverable) or HALT (latched)
```

`FAULT` allows clear via `CMD_RESET_FAULT`; `HALT` requires power cycle.

Sub-states inside `MOVING`: `SOFT_START`, `BACKLASH_CROSS`, `CRUISE`, `DECEL`, `CREEP_TO_CONTACT`.

---

## 5. Bus connectivity

### 5.1 CAN — vehicle bus role

Role: simulates the body-control-module → window-controller link. CWC is a **slave** that responds to commands from a master ECU (or a test harness pretending to be one).

- **Bitrate**: 500 kbps classic CAN (extensible to CAN-FD 2 Mbps data phase).
- **ID space**: 11-bit standard for v1; reserve 29-bit extended for future ChainTree-routed CAN.
- **Frame structure**:

```
ID  [10:7] priority   (0 = highest)
    [6:3]  node_addr  (CWC default = 0x4)
    [2:0]  msg_class  (0=cmd, 1=ack, 2=status, 3=event, 4=fault, 5=diag_req, 6=diag_resp, 7=reserved)
```

So a CMD to CWC node 4 with priority 2 = `0b010_0100_000` = `0x120`.

- **Periodic status**: msg_class=2, 50 Hz, 8-byte payload (see §6.3).
- **Events** (end-stop, obstruction, fault): msg_class=3 or 4, sent immediately, non-periodic.
- **Commands** (master → CWC): msg_class=0; CWC ACKs within 5 ms (msg_class=1) including command_id echo + result code.

### 5.2 RS485 — ChainTree role

Role: ChainTree multi-drop diagnostic and configuration bus. Per Glenn's existing ChainTree convention:

- **Bitrate**: 1 Mbps default, 8N1.
- **Framing**: SLIP escape (0xC0 frame delim, 0xDB escape).
- **Payload**: CBOR.
- **Addressing**: in-band polled, per ChainTree spec.
- **Map keys**: FNV-1a 32-bit hashes of canonical field names. Build-time registry generation with collision check.

CWC is a slave on this bus. Master polls; CWC responds.

---

## 6. Packet definitions

### 6.1 CAN ID scheme

(see §5.1)

CWC node_addr is configurable via RS485 (`CMD_SET_NODE_ADDR`), persisted to flash, default 0x4.

### 6.2 Master → CWC (CAN, msg_class=0)

Payload byte 0 = command code. Bytes 1–7 = command-specific.

| Code | Name | Bytes 1–7 |
|---|---|---|
| 0x01 | `CMD_MOVE_TO` | `[1:2]` target_pos (int16, encoder counts); `[3]` flags |
| 0x02 | `CMD_MOVE_DIR` | `[1]` direction (0=stop, 1=up, 2=down); `[2]` velocity scale (0–255, 255=v_max) |
| 0x03 | `CMD_STOP` | `[1]` 0=brake, 1=coast |
| 0x04 | `CMD_HOME` | `[1]` 0=auto-pick post, 1=force post A, 2=force post B |
| 0x05 | `CMD_RESET_FAULT` | — |
| 0x06 | `CMD_ENABLE` | `[1]` 0=disable (coast), 1=enable |
| 0x07 | `CMD_CALIBRATE` | `[1]` cal mode: 0=full ID, 1=backlash only, 2=friction only |
| 0x08 | `CMD_SET_AUTOMODE` | `[1]` 0=manual hold, 1=auto-up (one-touch), 2=auto-down |
| 0x10 | `CMD_DIAG_REQ` | `[1]` diag report code (see §6.4) — triggers RS485 emission |

Bytes unused = 0. CRC handled by CAN frame natively.

### 6.3 CWC → master (CAN)

**Periodic status (msg_class=2, 50 Hz):**

```
Byte 0      state         (uint8, enum: 0=INIT 1=HOMING 2=IDLE 3=MOVING 4=RETRACT 5=FAULT 6=HALT)
Byte 1      sub_state     (uint8, MOVING sub-states + flags)
Bytes 2–3   pos_output    (int16, encoder counts, signed from home)
Bytes 4–5   velocity      (int16, counts/sec)
Byte 6      current_mA_hi (uint8 — current in mA, 0–255 ≈ 0–255 mA)  // Note: scale fits expected 0–200 mA range
Byte 7      fault_flags   (uint8 — see §9)
```

For currents above 255 mA (transient inrush), saturate at 255 and rely on event packet for true peak.

**ACK (msg_class=1, on each command):**

```
Byte 0   command_code (echo)
Byte 1   result (0=OK, 1=BUSY, 2=PARAM_ERR, 3=STATE_ERR, 4=FAULT_BLOCKED)
Bytes 2–3   command_id (uint16, echo if master sent one in extended frames; else 0)
Bytes 4–7   result-specific data
```

**Event (msg_class=3):**

| Event code | Name | Payload |
|---|---|---|
| 0x01 | `EVT_ENDSTOP_HIT` | which post (1B), pos at contact (int16), peak current mA (uint16), traverse time ms (uint16) |
| 0x02 | `EVT_OBSTRUCTION` | pos (int16), current mA (uint16), ripple_freq Hz (uint16), confidence 0–255 (uint8) |
| 0x03 | `EVT_HOMED` | which post (1B), N_backlash measured (int16) |
| 0x04 | `EVT_AUTOSTOP_REACHED` | target_pos (int16), final_pos (int16), error counts (int16) |
| 0x05 | `EVT_CALIBRATION_DONE` | success flag (1B), R mΩ (uint16), Ke µV·s (uint16), J ×1e9 (uint16) |

**Fault (msg_class=4):** see §9 for fault flag layout.

### 6.4 RS485 diagnostic packages (CBOR over SLIP)

All map keys are FNV-1a 32-bit hashes of the canonical string. Canonical strings are listed below; registry generated at build time via `tools/gen_keys.py`.

**Diag report codes (referenced by `CMD_DIAG_REQ` and by master poll):**

| Code | Name | Canonical key root |
|---|---|---|
| 0x01 | `DIAG_FULL_SNAPSHOT` | `"diag.snapshot"` |
| 0x02 | `DIAG_RIPPLE_SPECTRUM` | `"diag.ripple_spectrum"` |
| 0x03 | `DIAG_WELFORD_STATS` | `"diag.welford"` |
| 0x04 | `DIAG_IDENT_PARAMS` | `"diag.ident_params"` |
| 0x05 | `DIAG_CYCLE_LOG` | `"diag.cycle_log"` |
| 0x06 | `DIAG_HEALTH_TREND` | `"diag.health_trend"` |
| 0x07 | `DIAG_EVENT_LOG` | `"diag.event_log"` |
| 0x08 | `DIAG_BUS_STATS` | `"diag.bus_stats"` |
| 0x09 | `DIAG_FW_INFO` | `"diag.fw_info"` |

**Schema — `DIAG_FULL_SNAPSHOT`:**

```
{
  fnv("ts_ms"):           uint64,
  fnv("state"):           uint8,
  fnv("sub_state"):       uint8,
  fnv("pos_motor"):       int32,
  fnv("pos_output"):      int32,
  fnv("v_meas"):          float32,
  fnv("v_d"):             float32,
  fnv("a_d"):             float32,
  fnv("i_mean"):          float32,
  fnv("i_ripple_amp"):    float32,
  fnv("ripple_freq"):     float32,
  fnv("ripple_count"):    int32,
  fnv("v_m_measured"):    float32,
  fnv("duty"):            float32,
  fnv("dir"):             int8,
  fnv("fault_flags"):     uint16,
}
```

**Schema — `DIAG_RIPPLE_SPECTRUM`:**

```
{
  fnv("ts_ms"):     uint64,
  fnv("f_center"):  float32,
  fnv("bins"):      [float32, float32, ...],  // 32 bins, log-magnitude
  fnv("snr_db"):    float32,
}
```

Computed offline on Core 0 from a circular buffer of the last 1024 ripple-band samples (via a one-shot 256-FFT, downsampled). Emitted on request or every minute of accumulated runtime.

**Schema — `DIAG_WELFORD_STATS`:**

```
{
  fnv("ts_ms"):       uint64,
  fnv("up.mean"):     float32,
  fnv("up.sigma"):    float32,
  fnv("up.n"):        uint32,
  fnv("down.mean"):   float32,
  fnv("down.sigma"): float32,
  fnv("down.n"):      uint32,
}
```

**Schema — `DIAG_IDENT_PARAMS`:**

```
{
  fnv("R"):              float32,   // Ω
  fnv("Ke"):             float32,   // V·s/rad
  fnv("Kt"):             float32,   // N·m/A  (Kt ≈ Ke in SI)
  fnv("J"):              float32,   // kg·m²
  fnv("N_backlash_up"):   int16,
  fnv("N_backlash_down"): int16,
  fnv("friction_lut_up"):   [float32 × 8],
  fnv("friction_lut_down"): [float32 × 8],
  fnv("N_poles"):        uint8,
  fnv("ident_ts_ms"):    uint64,
}
```

**Schema — `DIAG_CYCLE_LOG`:**

Ring of last 32 traverses:

```
{
  fnv("entries"): [
    {
      fnv("dir"):        int8,
      fnv("start_pos"):  int32,
      fnv("end_pos"):    int32,
      fnv("duration_ms"): uint32,
      fnv("peak_i_mA"):  uint16,
      fnv("mean_i_mA"):  uint16,
      fnv("end_reason"): uint8,  // 0=target, 1=endstop, 2=obstruction, 3=fault
    }, ...
  ]
}
```

**Schema — `DIAG_HEALTH_TREND`:**

Long-term EWMA deltas — useful for predictive maintenance:

```
{
  fnv("R_baseline"):       float32,
  fnv("R_recent"):         float32,
  fnv("R_pct_drift"):      float32,
  fnv("friction_baseline_norm"): float32,
  fnv("friction_recent_norm"):   float32,
  fnv("friction_pct_drift"):     float32,
  fnv("backlash_baseline"):  int16,
  fnv("backlash_recent"):    int16,
  fnv("brush_anomaly_score"): float32,  // 0..1, from ripple amplitude variance trend
  fnv("cycles_total"):     uint32,
  fnv("hours_run"):        float32,
}
```

**Schema — `DIAG_EVENT_LOG`:**

Ring of last 64 faults/events with timestamps; format mirrors the CAN event packet but with full uint64 ts and any truncated fields restored.

**Schema — `DIAG_BUS_STATS`:**

```
{
  fnv("can.tx"): uint32,
  fnv("can.rx"): uint32,
  fnv("can.err_passive"): uint32,
  fnv("can.bus_off"): uint32,
  fnv("can.rx_overflow"): uint32,
  fnv("rs485.tx"): uint32,
  fnv("rs485.rx"): uint32,
  fnv("rs485.crc_err"): uint32,
  fnv("rs485.frame_err"): uint32,
}
```

**Schema — `DIAG_FW_INFO`:**

```
{
  fnv("fw_version"): "x.y.z",
  fnv("git_sha"):    "...",
  fnv("build_ts"):   uint64,
  fnv("hw_rev"):     uint8,
  fnv("node_addr"):  uint8,
  fnv("uptime_s"):   uint32,
}
```

**RS485 master commands (CBOR map):**

| Key | Action |
|---|---|
| `fnv("cmd.diag_req")`: code | emit named diagnostic package |
| `fnv("cmd.set_param")`: {key:value, ...} | live-set runtime params (subject to whitelist) |
| `fnv("cmd.set_node_addr")`: uint8 | persist new CAN/RS485 node address |
| `fnv("cmd.start_ident")`: mode | trigger param-ID cycle |
| `fnv("cmd.firmware_reboot")`: 1 | soft reboot |
| `fnv("cmd.flash_save")`: 1 | force persistence flush |
| `fnv("cmd.flash_reset")`: 1 | factory reset (clear all persisted state) |

Whitelist for `cmd.set_param`: `v_max[dir]`, `a_max[dir]`, `j_max[dir]`, `v_creep[dir]`, `welford_k_threshold`, `obstruction_confirm_ms`. Not whitelisted: R, Ke, Kt, J — those come from ident only.

Unknown hash key → CWC posts an exception to `thread.exception` (per Glenn's ChainTree convention) and ignores the field.

---

## 7. Parameter identification

Triggered on first boot (no persisted params present) or via `CMD_CALIBRATE` / `cmd.start_ident`.

Sequence:

1. **Cold-stall R measurement.** Drive at 30% duty into post A. Steady-state V_M and I_mean → `R = duty·V_M / I_mean`. Should match nominal 48 Ω at room temp; flag if deviates > 20%.
2. **Free-spin Ke measurement.** Reverse from post A. *During the backlash window* (unloaded motor) drive at fixed 50% duty. Wait 100 ms for v_meas to settle. `Ke = (duty·V_M − i_mean·R) / ω_meas`, where `ω_meas = 2π·ripple_freq / N_poles`. If `N_poles` unknown, sweep candidates {3, 5} and pick the one giving Ke within tolerance of `(stall_torque × ω_noload)/(duty·V_M)` from datasheet expectation; fail to identify → require manual entry via RS485.
3. **Inertia J.** Apply a known torque step (constant `i_d`) for 50 ms; fit acceleration from ripple PLL derivative. `J = Kt·i_d / a_meas`.
4. **Friction LUT.** Drive at 8 fixed velocities (10%, 20%, ..., 80% of nominal v_max) in each direction. Steady-state: `τ_friction = duty·V_M/R·Kt − Kt²·v_meas/R` (or equivalently `i_meas·Kt − J·a` where a≈0 at steady state). Record per direction.
5. **Backlash widths.** During each of the four direction-reversal events in the ID sequence, record `N_backlash_meas`. Average for each direction.

Persist results to flash as `ident_params_t`. Emit `EVT_CALIBRATION_DONE` on CAN.

**Re-identification policy:** opportunistic. During normal operation, recompute R from steady-state during each cruise phase (V_M, i_mean, v_meas all known); EWMA update with α=0.001. If `R_recent / R_baseline > 1.30` → raise `WARN_R_DRIFT` (likely thermal or brush wear).

---

## 8. Persistence

Flash region: top 4 KB sector of RP2350 flash, with a simple A/B journaled scheme (write to B, verify, mark A invalid). Single struct:

```c
typedef struct {
    uint32_t magic;          // 0xC1D0DE00
    uint16_t version;
    uint16_t crc16;
    uint8_t  node_addr;
    uint8_t  hw_rev;
    ident_params_t ident;
    traj_profile_t profile_up;
    traj_profile_t profile_down;
    welford_state_t welford_up;
    welford_state_t welford_down;
    int32_t last_known_pos_output;
    uint32_t cycle_count;
    uint32_t runtime_seconds;
} persist_t;
```

Write triggers:
- Successful `STOPPED_AT_END`.
- `EVT_CALIBRATION_DONE`.
- `cmd.flash_save`.
- Every 60 s if any field has changed AND state is `IDLE`.

Never write during `MOVING`.

Boot: load persist, verify magic + CRC; on mismatch → run param-ID, then save.

---

## 9. Fault model

Single `uint16` `fault_flags`. Multiple can be set simultaneously. Bit layout:

| Bit | Name | Severity | Recovery |
|---|---|---|---|
| 0 | `FAULT_OVERCURRENT` | hard | brake, coast, → FAULT, allow reset |
| 1 | `FAULT_ENCODER_STALL` | hard | coast, → FAULT |
| 2 | `FAULT_ENCODER_RIPPLE_MISMATCH` | soft | log, continue, prefer ripple position |
| 3 | `FAULT_RIPPLE_LOST` | soft | log, fall back to encoder-only |
| 4 | `FAULT_VM_OUT_OF_RANGE` | hard | coast, → FAULT (battery sag or over-volt) |
| 5 | `FAULT_THERMAL_DERATE` | soft | reduce v_max, raise WARN |
| 6 | `FAULT_PERSIST_CRC` | soft on boot | force ID cycle |
| 7 | `FAULT_PARAM_MISSING` | hard on boot | require ID, → HALT until done |
| 8 | `FAULT_CAN_BUS_OFF` | soft | auto-recover per CAN PHY, log |
| 9 | `FAULT_RS485_FRAMING` | soft | drop frame, increment counter |
| 10 | `FAULT_BRIDGE_FAULT` | hard | DRV8838 has no nFAULT pin — inferred from sudden current loss under commanded duty → HALT |
| 11 | `FAULT_POSITION_DRIFT` | soft | log, schedule rehome at next IDLE |
| 12 | `FAULT_SLIP_DETECTED` | soft | log, reduce v_max, schedule rehome |
| 13 | `FAULT_OBSTRUCTION_REPEATED` | hard | 3 obstructions on same traverse within 5 cycles → → HALT, require master ack |
| 14 | `FAULT_COMMAND_TIMEOUT` | soft | revert to IDLE |
| 15 | reserved | | |

**Exception posting:** for any fault, emit:
1. CAN event (msg_class=4) with code + context, immediate.
2. RS485 entry into `DIAG_EVENT_LOG`.
3. ChainTree exception to `thread.exception` topic if RS485 bus master is a ChainTree node — payload includes hash key, timestamp, fault flags, last setpoint, last telemetry.

`CMD_RESET_FAULT` clears soft + hard fault bits; HALT bits require power cycle (no command unlatches).

---

## 10. Build & deployment notes

- **Toolchain**: pico-sdk 2.x with M33 + DSP + FPU enabled. CMake.
- **Source layout**:

```
cwc/
├── CMakeLists.txt
├── continue.md                  # this file
├── include/
│   ├── cwc_config.h            # all #defines, pin map, defaults
│   ├── packet_defs.h           # CAN + CBOR packet structs
│   └── fnv_keys.h              # GENERATED — do not edit
├── src/
│   ├── main.c                  # init, core0 entry
│   ├── core1.c                 # 25 kHz inner loop
│   ├── bridge.c                # DRV8838 control
│   ├── adc_dma.c               # ADC + PIO sample trigger
│   ├── encoder.c               # PIO quadrature
│   ├── ripple.c                # Goertzel + PLL
│   ├── welford.c               # per-direction stats
│   ├── scurve.c                # trajectory generator
│   ├── feedforward.c           # torque map
│   ├── state.c                 # state machine
│   ├── ident.c                 # parameter ID
│   ├── persist.c               # flash A/B journal
│   ├── can.c                   # MCP2517FD driver + msg framing
│   ├── rs485.c                 # SLIP + CBOR + ChainTree
│   ├── fault.c                 # fault flag dispatch
│   └── log.c                   # event log ring
├── tools/
│   ├── gen_keys.py             # build-time FNV-1a registry
│   ├── canbus_probe.py         # host tool: sniff/inject CAN
│   └── chaintree_diag.py       # host tool: poll RS485 diag packages
└── test/
    ├── unit/                   # hostable unit tests for math (scurve, welford, fnv)
    └── hil/                    # hardware-in-loop scripts
```

- **`gen_keys.py`**: scans source for `FNV("string")` macro invocations, generates `fnv_keys.h` with `#define FNV_<UPPER> 0xXXXXXXXX` per key, checks for collisions, fails build on collision.
- **Defaults config**: every tunable lives in `cwc_config.h` with a `#define DEFAULT_*` and is overridable via flash-persisted `persist_t` at boot.
- **Logging**: a lightweight ring buffer drained by RS485 when bus master polls; never blocks the 25 kHz loop.
- **No dynamic allocation** anywhere in the 25 kHz path or 1 kHz path. All buffers statically sized in `cwc_config.h`.

**Bring-up order for Claude Code:**

1. Pin map + skeleton, blink LED.
2. PWM out → DRV8838 → drive motor open-loop both directions.
3. ADC + DMA + INA240 read; print mean current at known duty.
4. PIO encoder; verify pulse counts vs. expected.
5. RS485 SLIP/CBOR loopback to host tool.
6. CAN TX of fixed status frame; verify on bus analyzer.
7. Ripple high-pass + Goertzel; sweep duty, verify ripple_freq tracks RPM.
8. Welford on Core 1; expose via diag snapshot.
9. S-curve generator on Core 0; setpoint hand-off to Core 1.
10. Feedforward map; verify zero-load no-jerk motion.
11. Homing routine on foam post.
12. Backlash crossing logic.
13. Anti-pinch obstruction detection.
14. Param ID cycle.
15. Persistence A/B journal.
16. Fault injection + recovery testing.

Each step has a hostable test or a defined success criterion before advancing.

---

## 11. Open questions / deferred decisions

1. **N_poles** of the specific N20 sample: confirm with scope on first hardware. Spec assumes 3; sweep {3, 5} during ID if uncertain.
2. **CAN-FD vs classic CAN**: default classic for v1. Promote to FD once the master test harness supports it (would let us push full diag snapshots over CAN instead of RS485 for some deployments).
3. **Quadrature vs single-edge encoder counting**: both A and B wired; default to quadrature for direction-redundant counting. Falls back to single-edge with commanded-direction signing if B fails.
4. **RS485 master identity**: assumed to be a ChainTree gateway, but spec must work standalone (test harness). Diag schema independent of master role.
5. **Thermal sensor**: optional ADC2 input; if absent, derive temperature from `R` drift relative to baseline (less accurate but free).
6. **Self-test on boot**: minimal version is "current = 0 with motor disabled, ADC reads near 0"; full version drives at 10% in each direction for 100 ms with current bounds. Decide which is default; suggest full self-test if state is `INIT` with no recent persisted activity.
7. **Bridge fault inference**: DRV8838 has no fault output. Detect by: commanded duty > 0 AND current < expected_min for > 50 ms AND no encoder motion → infer fault. May produce false positives at low duty / cold start; tune thresholds during HIL.
8. **Soft "auto-up" anti-pinch tuning**: US FMVSS 118 specifies max 100 N pinch force. We don't measure force directly; correlate `i_mean − no_load_baseline` to load torque via ident, then to apparent contact force via gear ratio + drum radius. Validate during HIL with a calibrated force gauge.

---

## Appendix A — Glossary of canonical keys (incomplete; full list in `tools/gen_keys.py`)

```
diag.snapshot, diag.ripple_spectrum, diag.welford, diag.ident_params,
diag.cycle_log, diag.health_trend, diag.event_log, diag.bus_stats, diag.fw_info,
cmd.diag_req, cmd.set_param, cmd.set_node_addr, cmd.start_ident,
cmd.firmware_reboot, cmd.flash_save, cmd.flash_reset,
ts_ms, state, sub_state, pos_motor, pos_output, v_meas, v_d, a_d,
i_mean, i_ripple_amp, ripple_freq, ripple_count, v_m_measured, duty, dir,
fault_flags, up.mean, up.sigma, up.n, down.mean, down.sigma, down.n,
R, Ke, Kt, J, N_backlash_up, N_backlash_down, friction_lut_up, friction_lut_down,
N_poles, ident_ts_ms, entries, start_pos, end_pos, duration_ms,
peak_i_mA, mean_i_mA, end_reason, R_baseline, R_recent, R_pct_drift,
friction_baseline_norm, friction_recent_norm, friction_pct_drift,
backlash_baseline, backlash_recent, brush_anomaly_score,
cycles_total, hours_run, can.tx, can.rx, can.err_passive, can.bus_off,
can.rx_overflow, rs485.tx, rs485.rx, rs485.crc_err, rs485.frame_err,
fw_version, git_sha, build_ts, hw_rev, node_addr, uptime_s,
f_center, bins, snr_db
```

End of continue.md
