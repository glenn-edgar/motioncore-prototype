# continue.md — ESP32 Motion Subsystem for ChainTree Diff-Drive Prototype

**Status:** design captured 2026-05-02
**Target hardware:** ESP32 (or ESP32-S3) driver board, 2× ST3215 serial bus servos as wheels, auto-direction RS-485 transceiver, Pi 5 host
**Forward target:** FIRST Motioncore + A301 actuators (2027-28 FTC season, 2026-27 FRC)

---

## 1. Architectural recap

The ESP32 is a bus master for the wheel subsystem and presents a unified register-mapped interface upward to the host. This mirrors the Motioncore/Systemcore split FIRST is shipping: the host runs planning and high-level Drive Programs, the ESP32 runs the millisecond-scale reactive loop and aggregates state.

```
Host (Pi 5, ChainTree top-level + Drive Programs)
   ↓  upstream link (USB-CDC for dev; RS-485 slave for production)
ESP32 motion subsystem (this document)
   ├── UART1 → SCS bus → ST3215 left wheel
   │                    → ST3215 right wheel
   └── UART2 → RS-485 → sensor nodes (future: MG24 mesh, ToF pods)
```

**Why this split is correct:**

- It matches Motioncore semantically. When A301 ships, swap the wheel-bus driver from SCS-over-UART to CAN-to-A301 and the upstream interface is unchanged.
- The ESP32 owns the timing-critical loop. Host jitter from Linux scheduling is irrelevant to wheel control.
- Welford anomaly detection runs naturally at the bus master, where current readings are already arriving at full rate.
- ChainTree's Level 1 reactive layer can run on the ESP32, leaving Level 2 planning on the host. The bus boundary becomes the level boundary.

---

## 2. ESP32 firmware structure

FreeRTOS, three primary tasks plus the UART ISRs:

| Task | Rate | Priority | Purpose |
|---|---|---|---|
| `uplink_task` | event-driven | medium | Service host commands; serialize/deserialize register frames |
| `motion_task` | 100 Hz | high | Diff-drive kinematics; SCS bus commands; per-wheel telemetry read |
| `anomaly_task` | 50 Hz | medium | Welford on motor current; set fault bits; emit anomaly events |
| `sensor_task` | 50 Hz (future) | medium | Poll RS-485 sensor bus; aggregate into pose estimate |
| `heartbeat_task` | 10 Hz | low | Watchdog feed; status LED; OLED if present |

UARTs are configured with DMA-backed ring buffers. Critical: **only `motion_task` writes to the wheel bus.** Other tasks request reads via a queue. This serializes bus access without explicit mutex churn on the high-frequency path.

### 2.1 Task: `motion_task` (the heart of the subsystem)

100 Hz loop, period 10 ms. Each tick:

1. Read latest setpoint from command register block (atomic snapshot of `linear_v`, `angular_v`, mode)
2. Apply diff-drive kinematics:
   - `v_left  = linear_v - (angular_v * wheelbase / 2)`
   - `v_right = linear_v + (angular_v * wheelbase / 2)`
3. Convert m/s → servo speed units using `wheel_radius` and ST3215 speed scale
4. Issue **sync write** to both servos (one bus transaction, both speed setpoints)
5. Issue **sync read** for position, speed, load, current from both servos
6. Update telemetry register block with parsed feedback
7. Update odometry estimate (integrate per-wheel speed × dt)
8. Push current samples into Welford accumulator queue for `anomaly_task`

**Sync read/write is essential.** Don't issue per-servo transactions sequentially — the SCS protocol's sync commands let you address both servos in one bus exchange. This holds the wheel pair in tight lockstep and halves bus utilization.

**Timing budget per tick (1 Mbps SCS bus):**

- Sync write to 2 servos: ~16 bytes → 160 µs
- Sync read response from 2 servos: ~32 bytes → 320 µs
- Inter-frame gaps + safety margin: ~200 µs
- Kinematics + odometry math: <100 µs on Cortex-M0 equivalent
- Total: ~800 µs of a 10 ms tick → 8% bus utilization, plenty of headroom

### 2.2 Task: `anomaly_task`

Welford running estimator per wheel current channel. State:

```c
typedef struct {
    uint32_t n;
    float    mean;
    float    M2;          // sum of squared deviations
    float    last_z;
    bool     fault_active;
} welford_t;
```

Update at 50 Hz from motion_task's sample queue. After warmup (n > 30):

- z = (sample - mean) / stddev
- |z| > 2.0 for ≥3 consecutive samples → set `fault_active`, set fault bit in telemetry register, emit asynchronous fault frame upstream
- |z| < 1.0 for ≥10 consecutive samples → clear `fault_active`

This is the same pattern as the anomalisa paper, just embedded. State lives entirely in 32 bytes per wheel — fits trivially.

**Reset semantics:** clear Welford state on `configure()` command from host. Otherwise, a config change (different wheel radius, different surface) leaves stale statistics that misfire anomalies.

### 2.3 Task: `uplink_task`

Services host link. SLIP-framed command frames, FNV-1a dispatch. Verbs are intentionally tiny:

```
0x... set_velocity        (i32 linear_mm_s, i32 angular_mrad_s)
0x... set_wheel_velocity  (i32 left_mm_s, i32 right_mm_s)
0x... emergency_stop      ()
0x... configure           (config_blob)
0x... read_telemetry      ()                       — pull mode
0x... reset_anomaly       ()
0x... ping                ()
```

Telemetry is **also pushed** at 50 Hz unsolicited, separate from `read_telemetry`. The pull verb is for diagnostic tools; production integration uses the push stream. This eliminates host polling latency from the control loop.

Push frame schema (fixed 64 bytes):

```
seq        u32   monotonic counter
timestamp  u32   ESP32 µs since boot
pose_x     i32   mm
pose_y     i32   mm
pose_theta i32   mrad
left_vel   i16   mm/s
right_vel  i16   mm/s
left_curr  i16   mA
right_curr i16   mA
left_temp  u8    °C
right_temp u8    °C
voltage    u16   mV
fault_flags u16
crc        u16
```

CRC-16 over the body, before SLIP framing. Drop-detection on the host via seq.

---

## 3. Register table (the upstream contract)

Even with custom framing, keep register-table semantics. This is the abstraction that survives the swap to A301-on-CAN.

```
COMMAND BLOCK      (host writes; ESP32 reads)
0x0000  command_word        u16   bit0=enable, bit1=estop, bit2=reset_odom, ...
0x0002  control_mode        u8    0=velocity, 1=wheel_direct, 2=pose_target
0x0010  linear_v_setpoint   i32   mm/s
0x0014  angular_v_setpoint  i32   mrad/s
0x0020  left_v_setpoint     i32   mm/s   (wheel_direct mode)
0x0024  right_v_setpoint    i32   mm/s

CONFIG BLOCK       (host writes; ESP32 reads, persists in NVS)
0x0100  wheelbase_mm        u16
0x0102  wheel_radius_mm     u16
0x0104  max_linear_v        u16   mm/s
0x0106  max_angular_v       u16   mrad/s
0x0110  pid_kp              float
0x0114  pid_ki              float
0x0118  pid_kd              float
0x0120  anomaly_z_threshold float
0x0124  anomaly_min_samples u16

TELEMETRY BLOCK    (ESP32 writes; host reads)
0x0200  pose_x              i32   mm
0x0204  pose_y              i32   mm
0x0208  pose_theta          i32   mrad
0x0210  left_velocity       i16   mm/s
0x0212  right_velocity      i16   mm/s
0x0214  left_position       i32   ticks
0x0218  right_position      i32   ticks
0x0220  left_current        i16   mA
0x0222  right_current       i16   mA
0x0224  left_load           u8
0x0225  right_load          u8
0x0226  left_temp           u8    °C
0x0227  right_temp          u8    °C
0x0228  bus_voltage         u16   mV
0x022A  fault_flags         u16   bit0=l_anomaly, bit1=r_anomaly, ...
0x022C  heartbeat_counter   u32

INFO BLOCK         (read-only)
0x0300  firmware_version    u32
0x0304  hardware_id         u32
0x0308  uptime_seconds      u32
```

ChainTree drivers see this as `read_register(addr, len)` / `write_register(addr, data)`. That abstraction maps to:

- This prototype's SLIP+FNV custom protocol → register table on ESP32
- Future Modbus RTU on the same RS-485 bus → register table on ESP32
- Future A301-on-CAN → CAN device tables on Motioncore

Same driver interface, three different bus implementations.

---

## 4. SCS bus configuration (downward to ST3215)

- **Baud:** 1 Mbps (default, leave at this)
- **UART hardware:** UART1 on ESP32, TX-only with auto-direction transceiver, RX as a separate pin (the SCS bus is half-duplex but the transceiver handles direction)
- **Servo IDs:** 1 = left, 2 = right. Set via Feetech debug tool before assembly. ID 1 is factory default — bring up ONE servo at a time when assigning IDs to avoid bus conflict.
- **Operating mode:** wheel/motor mode, register 0x21 = 1. Persist to EPROM. Verify on every boot via read-back; refuse to start if either wheel comes up in position mode.
- **Acceleration:** set the on-servo accel ramp (register 0x29) to a moderate value. Don't rely on the host for ramp shaping — the servo's internal trapezoidal profile is smoother than anything we'd impose from the 100 Hz loop.

---

## 5. Driver porting analysis

### 5.1 Existing C/C++ drivers

| Library | Source | Form | License | Notes |
|---|---|---|---|---|
| FTServo_Arduino (official) | github.com/ftservo/FTServo_Arduino | C++ Arduino lib | (Feetech) | Reference implementation; Arduino-only |
| workloads/scservo | github.com/workloads/scservo | C++ Arduino + ESP32 | GPL-3.0 | Fork of Waveshare ugv_base_general; Arduino IDE friendly |
| parallax/scservo | github.com/parallax/scservo | C++ Arduino + ESP32 | GPL-3.0 | Same lineage as workloads/scservo |
| matthieuvigne/STS_servos | github.com/matthieuvigne/STS_servos | C++ Arduino | MIT | Cleanest API; STS3215-focused; well documented |
| adityakamath/SCServo_Linux | github.com/adityakamath/SCServo_Linux | C++ Linux | (Feetech) | Linux SDK with sync read/write, multi-mode, examples |
| feetech-servo-sdk | pypi.org/project/feetech-servo-sdk | Python | (Feetech) | Reference for protocol semantics if porting from scratch |
| LeRobot motors module | github.com/huggingface/lerobot | Python | Apache-2.0 | ROS2-style abstraction; useful as architectural reference |

### 5.2 Recommended porting approach for ESP32

**Option A — Use Arduino-ESP32 framework, drop in workloads/scservo or matthieuvigne/STS_servos as-is**

- Pros: Works immediately. SCServo and HardwareSerial APIs map directly. Most existing examples compile unchanged.
- Cons: Arduino layer adds latency and indirection. Cooperative scheduling. Harder to integrate with FreeRTOS tasks the way we need.
- Use this for first-light validation. Replace before production.

**Option B — Port to ESP-IDF native (recommended)**

The actual SCS protocol is small. The cleanest path is to take the protocol logic from the existing libraries and write a thin ESP-IDF UART wrapper. Keep the protocol code, replace the I/O layer.

What to extract from existing libraries:

- Packet structure: header bytes (0xFF 0xFF), ID, length, instruction, parameters, checksum
- Instructions: PING (0x01), READ (0x02), WRITE (0x03), REG_WRITE (0x04), ACTION (0x05), SYNC_WRITE (0x83), SYNC_READ (0x82)
- Register table for ST3215 (model-specific addresses for goal_position, goal_speed, present_position, present_load, present_voltage, present_current, present_temperature, mode, etc.)
- Checksum: ~(sum of bytes from ID through last param) & 0xFF

What to write fresh for ESP-IDF:

```c
// scs_bus.h — minimal driver surface
typedef struct scs_bus scs_bus_t;

scs_bus_t* scs_bus_create(uart_port_t port, int tx_pin, int rx_pin, int baud);
esp_err_t  scs_bus_destroy(scs_bus_t*);

esp_err_t scs_ping(scs_bus_t*, uint8_t id);
esp_err_t scs_read(scs_bus_t*, uint8_t id, uint8_t addr, uint8_t len, uint8_t* out);
esp_err_t scs_write(scs_bus_t*, uint8_t id, uint8_t addr, uint8_t len, const uint8_t* data);
esp_err_t scs_sync_write(scs_bus_t*, uint8_t addr, uint8_t len,
                         const uint8_t* ids, uint8_t n_ids,
                         const uint8_t* data);
esp_err_t scs_sync_read(scs_bus_t*, uint8_t addr, uint8_t len,
                        const uint8_t* ids, uint8_t n_ids,
                        uint8_t* out);
```

Internally, this uses `uart_write_bytes` and `uart_read_bytes` with a per-bus mutex. Auto-direction transceivers mean no DE/RE handling. Total port effort: estimated 1–2 days.

A higher-level wheel driver wraps `scs_bus_t` with semantic operations:

```c
// st3215_wheel.h
typedef struct {
    scs_bus_t* bus;
    uint8_t    id;
    int16_t    last_velocity;
    int32_t    last_position;
    int16_t    last_current;
    // ... cached state
} st3215_wheel_t;

esp_err_t st3215_set_mode_wheel(st3215_wheel_t*);
esp_err_t st3215_set_velocity(st3215_wheel_t*, int16_t units);
esp_err_t st3215_read_state(st3215_wheel_t*);  // updates cached fields
```

The wheel pair gets a synchronized helper that issues one bus transaction:

```c
esp_err_t st3215_pair_set_velocity(st3215_wheel_t* left, st3215_wheel_t* right,
                                    int16_t v_left, int16_t v_right);
esp_err_t st3215_pair_read_state(st3215_wheel_t* left, st3215_wheel_t* right);
```

These call `scs_sync_write` / `scs_sync_read` under the hood.

**Option C — Bring up via the workloads/scservo fork, then incrementally rewrite**

Practical path: get to first wheel motion using Option A, validate the protocol against actual servos, then peel back to Option B's ESP-IDF native layer once you understand the bus well enough to be selective.

### 5.3 Forward path to A301

The wheel driver layer is the only thing that changes when A301 arrives. Replace `st3215_wheel.h` with `a301_wheel.h`, where the implementation talks CAN frames to Motioncore instead of UART frames to the SCS bus. The motion task's call sites are unchanged because they only use the semantic API (`set_velocity`, `read_state`, `pair_set_velocity`).

This is the architectural pivot we're paying for now. The ChainTree integration above the register table doesn't change at all.

---

## 6. RS-485 upstream (production link)

USB-CDC is fine for development. Production link is RS-485 with the host as master.

- **Baud:** 460800 or 921600 (well within auto-direction transceiver limits)
- **Framing:** SLIP (already in toolkit), 1-byte address prefix, 2-byte CRC-16, body, end-of-frame
- **Address allocation:** 0x01 = motion subsystem, 0x02 = arm subsystem (future), 0x03 = sensor concentrator, etc.
- **Transaction model:** host commands → ESP32 ACK with status. Telemetry pushed unsolicited on schedule, with the host's polling loop just receiving and parsing.
- **Auto-direction transceivers:** no DE/RE GPIO. Driver is symmetric to a normal UART. Verify failsafe biasing on the bus (1 kΩ pull-up on A, pull-down on B at the master end) to keep idle line state defined.

When the production board has both a USB-C and an RS-485 port, support both upstream links from the same firmware build, selectable at boot via GPIO strap or boot-time configuration register.

---

## 7. Open questions / decisions to lock down

- [ ] Default control loop rate: 100 Hz seems right for diff-drive at this scale. Confirm with first-light timing measurements.
- [ ] Welford z-threshold: starting at 2.0 per anomalisa default; tune after collecting 1 hour of nominal driving data.
- [ ] Pose estimation: dead-reckoning from wheel encoders is the v1 plan. IMU fusion via secondary sensor bus (UART2) is v2.
- [ ] Persistence: save config to ESP32 NVS on `configure()` write. On boot, validate against firmware version; reset to defaults on mismatch.
- [ ] Watchdog: feed from `heartbeat_task`; if `motion_task` misses N consecutive ticks, esp_restart() with reason logged to NVS.
- [ ] Emergency stop: should set both wheel velocities to 0 AND set torque-enable register to 0 on both servos (release the wheels). Document the recovery procedure.
- [ ] OTA firmware update: defer to post-prototype. Use ESP-IDF OTA partitions when ready.
- [ ] Test harness: write a Python host-side mock that drives the register table over USB-CDC, captures telemetry, and produces the same logs format as the real host. This is the regression substrate.

---

## 8. Sequence of work (suggested)

1. **First light** (1–2 days): Arduino-ESP32 with workloads/scservo; one servo, set_velocity, read_position, log to USB serial.
2. **Pair sync** (1 day): two servos with sync_write/sync_read; verify both turn at commanded speeds.
3. **Mechanical prototype** (1 day): 3D-print LeKiwi-style drive motor mounts adapted for diff-drive (skip the third wheel); add caster; test motion under no-load.
4. **Register table + uplink** (2–3 days): port to ESP-IDF, FreeRTOS task structure, USB-CDC SLIP+FNV protocol, register table.
5. **Welford + faults** (1 day): integrate anomaly detection on current; verify by stalling a wheel.
6. **Host integration** (2–3 days): ChainTree driver speaking the register table; first Drive Program executing diff-drive motion via behavior tree.
7. **RS-485 upstream** (1 day): swap USB-CDC for RS-485 master/slave; verify production topology.
8. **Documentation** (ongoing): update this continue.md as decisions firm up.

---

## 9. References

- Waveshare ST3215 wiki: https://www.waveshare.com/wiki/ST3215_Servo
- Feetech FTServo_Arduino: https://github.com/ftservo/FTServo_Arduino
- workloads/scservo (ESP32-friendly fork): https://github.com/workloads/scservo
- matthieuvigne/STS_servos (clean API): https://github.com/matthieuvigne/STS_servos
- SCServo_Linux SDK (sync read/write reference): https://github.com/adityakamath/SCServo_Linux
- LeKiwi (3D-printable mounts for ST3215): https://github.com/SIGRobotics-UIUC/LeKiwi
- FIRST control system update (forward target): https://community.firstinspires.org/control-system-update-first-tech-challenge-edition
- FIRST A301 actuator: https://community.firstinspires.org/introducing-the-first-a301
- ESP-IDF UART driver: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/uart.html
- ESP-IDF FreeRTOS: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/freertos.html

