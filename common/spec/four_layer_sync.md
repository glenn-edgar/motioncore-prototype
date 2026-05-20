# Four-Layer Sync Protocol — Dongle ↔ Linux Host

**Status:** Phase 2h spec (2026-05-19). Locked through 2026-05-16 design dialog + 2026-05-19 opcode-allocation dialog. Not yet implemented in firmware.

**Scope:** USB-CDC libcomm wire between a dongle (SAMD21/RA4M1/RP2350/ESP32-C6) and its Linux host. Slave↔router-dongle RS-485 wire is a parallel spec (see [rs485_slave_protocol_2026-05-16.md](../../../.claude/projects/-home-gedgar-motioncore-prototype/memory/rs485_slave_protocol_2026-05-16.md) memory) — the four-layer model is universal, but this document covers only the libcomm half.

**Supersedes:** the "Phase 2h edit `send_register` spike" plan from `dongle_class_identity_2026-05-13`. The REGISTER v2 payload still applies but now lives inside the 3-message sync ladder defined here.

---

## 1. The four layers

Layers are **semantic**, not protocol-specific. The same model applies to slave↔router-dongle; only the carrier differs.

| L | Name | Purpose | Dongle↔Linux carrier |
|---|---|---|---|
| L0 | Commissioning | Persist class/instance identity to flash | `OP_COMMISSION_SET` / `OP_COMMISSION_CLEAR` / `OP_COMMISSION_REPLY` |
| L1 | Identity | Declare who-I-am; bind to host's robot-config role | `OP_REGISTER` (v2 payload with `class_id` + `instance_id` + `commissioning_state`) / `OP_REGISTER_ACK` |
| L2 | Manifest | Declare runtime opcode catalog | `OP_GET_MANIFEST` / `OP_MANIFEST_REPLY` |
| L3 | Topology | Push downstream-bus inventory | `OP_SLAVE_REGISTER` × N — **router-only**; leaf dongles NAK with `err_unsupported_cmd` |

**Leaf vs router:** SAMD21 and RA4M1 are leaf-only (no L3). RP2350 and ESP32-C6 build for either role; the dongle build can be router-capable, the slave build never is.

---

## 2. Dongle state machine

```
                       ┌──────────────────────────────────┐
                       │ UNCOMMISSIONED                   │
                       │                                  │
                       │ Accepts: OP_COMMISSION_SET only  │
                       │ NAKs:    everything else         │
                       │          (err_state)             │
                       │                                  │
                       │ On OP_COMMISSION_SET:            │
                       │   write flash → OP_COMMISSION_   │
                       │   REPLY → reboot_with_cause(     │
                       │     REBOOT_CAUSE_COMMISSIONING)  │
                       └──────────────┬───────────────────┘
                                      │ reboot — flash now
                                      │ says COMMISSIONED
                                      ▼
                       ┌──────────────────────────────────┐
                       │ BOOT                             │
                       │                                  │
                       │ Periodic OP_REGISTER retry       │
                       │ (~1 Hz; Phase 2f loop).          │
                       │ LED off; no heartbeats.          │
                       │                                  │
                       │ Accepts: OP_REGISTER_ACK         │
                       │          OP_COMMISSION_CLEAR     │
                       │          (any state)             │
                       │ NAKs:    everything else         │
                       │          (err_state)             │
                       │                                  │
                       │ On OP_REGISTER_ACK: → L1_DONE    │
                       └──────────────┬───────────────────┘
                                      │
                                      ▼
                       ┌──────────────────────────────────┐
                       │ L1_DONE                          │
                       │                                  │
                       │ Bound to host. No heartbeats yet.│
                       │                                  │
                       │ Accepts: OP_GET_MANIFEST         │
                       │            (no state change)     │
                       │          OP_SLAVE_REGISTER       │
                       │            (router only; leaves  │
                       │             NAK err_unsupported) │
                       │          OP_OPERATIONAL_BEGIN    │
                       │            → OPERATIONAL         │
                       │          OP_COMMISSION_CLEAR     │
                       │            → reboot              │
                       │ NAKs:    OP_REGISTER_ACK (done)  │
                       │          app-layer opcodes       │
                       │          (err_state)             │
                       └──────────────┬───────────────────┘
                                      │
                                      ▼
                       ┌──────────────────────────────────┐
                       │ OPERATIONAL                      │
                       │                                  │
                       │ Normal traffic: heartbeats,      │
                       │ PING/PONG, app opcodes,          │
                       │ OP_GET_MANIFEST refresh OK.      │
                       │                                  │
                       │ NAKs:    OP_REGISTER_ACK         │
                       │          OP_OPERATIONAL_BEGIN    │
                       │          (err_state)             │
                       │                                  │
                       │ EV_HOST_REATTACH → BOOT          │
                       │ OP_COMMISSION_CLEAR → reboot     │
                       └──────────────────────────────────┘
```

**Key rules:**

- **L2_DONE is NOT a distinct state.** Manifest pull is a read-only side activity inside L1_DONE. The only advance from L1_DONE is `OP_OPERATIONAL_BEGIN`.
- **Host drives every transition** with an explicit advance opcode. The dongle is otherwise passive — retries `OP_REGISTER` in BOOT, responds to host opcodes, NAKs everything inappropriate.
- **Host reattach (Phase 2g)** drops the dongle back to BOOT; commissioning persists, so the next `OP_REGISTER` carries the same identity.

---

## 3. Opcode catalog

### 3.1 Allocation rule (locked 2026-05-13)

| Range | Direction | Constraint |
|---|---|---|
| `0x0001` – `0x00FF` | s2m | dongle → host (frame_meta_t.cmd); never appears in engine event_queue |
| `0x0100` – `0xFEFF` | m2s | host → dongle; pushed into engine event_queue → **MUST avoid** `SE_EVENT_TICK=0x0004`, `SE_EVENT_INIT=0xfffe`, `SE_EVENT_TERMINATE=0xfffd` |
| `0xFE00` – `0xFEFF` | internal | engine-internal events; never on the wire |

### 3.2 Full opcode table

**Existing (Phase 2d–2g, in `samd21/apps/register_dongle/vendor/libcomm/opcodes.h`):**

| Opcode | Hex | Dir | Layer | Purpose |
|---|---|---|---|---|
| `OP_REGISTER` | `0x0001` | s2m | L1 | Dongle identity announcement (v2 payload) |
| `OP_HEARTBEAT` | `0x0002` | s2m | OPERATIONAL | Periodic alive ping |
| `OP_PONG` | `0x0005` | s2m | OPERATIONAL | Response to `OP_PING` |
| `OP_DBG_LOG` | `0x0010` | s2m | any | `se_log` text output (UTF-8 payload) |
| `OP_REGISTER_ACK` | `0x0103` | m2s | L1 | Host acknowledges `OP_REGISTER` → advances BOOT → L1_DONE |
| `OP_PING` | `0x0104` | m2s | OPERATIONAL | Host-initiated round-trip probe |

**Internal:**

| Event | Hex | Dir | Purpose |
|---|---|---|---|
| `EV_HOST_REATTACH` | `0xFE00` | internal | main.c detected CDC reattach → engine drops to BOOT |
| `EV_COMMISSION_SET` | `0xFE01` | internal | RX path saw `OP_COMMISSION_SET` → chain handles flash write |

**New for Phase 2h (this spec):**

| Opcode | Hex | Dir | Layer | Purpose |
|---|---|---|---|---|
| `OP_NAK` | `0x0007` | s2m | any | Generic state/permission error (struct in §4.5) |
| `OP_MANIFEST_REPLY` | `0x0008` | s2m | L2 | Manifest payload (struct in §4.4) |
| `OP_GET_MANIFEST` | `0x0107` | m2s | L2 | Host requests manifest |
| `OP_OPERATIONAL_BEGIN` | `0x0108` | m2s | L1→OPER | Advance L1_DONE → OPERATIONAL |

**L0 (commissioning — used only by the standalone production tool, §6):**

| Opcode | Hex | Dir | Layer | Purpose |
|---|---|---|---|---|
| `OP_COMMISSION_REPLY` | `0x0006` | s2m | L0 | Confirms flash write succeeded |
| `OP_COMMISSION_SET` | `0x0105` | m2s | L0 | Persist `instance_id` to flash |
| `OP_COMMISSION_CLEAR` | `0x0106` | m2s | L0 | Factory-reset commissioning page |

### 3.3 Allocation history

The commissioning slots `0x0006` / `0x0105` / `0x0106` were claimed by `dongle_class_identity_2026-05-13`. The 2026-05-19 dialog initially landed manifest/operational opcodes on the same slots; renumbered to `0x0008` / `0x0107` / `0x0108` to resolve. Recorded here so the renumber doesn't get re-litigated.

---

## 4. Payload layouts

All payloads are `#pragma pack(push, 1)` C structs. Little-endian on the wire (matches all four chip families' native endianness).

### 4.1 `OP_REGISTER` (s2m) — v2 payload

From `dongle_class_identity_2026-05-13`. 38 bytes; fits one 128-byte libcomm frame.

```c
typedef struct {
    uint8_t  version;              //  1   payload version (=2)
    uint32_t class_id;             //  4   fnv1a(class_name) — compile-time constant
    uint32_t instance_id;          //  4   0 if uncommissioned, else commissioned id
    uint8_t  commissioning_state;  //  1   0=UNCOMMISSIONED, 1=COMMISSIONED
    uint8_t  chip_uid[16];         // 16   silicon factory id (diagnostic; not routing)
    uint16_t vid;                  //  2
    uint16_t pid;                  //  2
    uint32_t fw_version;           //  4   build hash, or build_date packed YYYYMMDD
    uint32_t build_date;           //  4   packed YYYYMMDD (open: drop in favor of fw_version?)
} op_register_payload_v2_t;        // = 38 bytes
```

### 4.2 `OP_REGISTER_ACK` (m2s)

Empty payload. Receipt alone advances the state.

### 4.3 `OP_GET_MANIFEST` (m2s)

Empty payload. Receipt triggers `OP_MANIFEST_REPLY`; does not change state.

### 4.4 `OP_MANIFEST_REPLY` (s2m)

```c
typedef struct {
    uint32_t schema_hash;        //  4   fnv1a over the schema-definition string
    uint32_t firmware_version;   //  4   (major<<16) | (minor<<8) | patch
    uint8_t  m2s_opcode_count;   //  1   N below
    // followed by:
    // uint16_t m2s_opcodes[N];        2*N
} op_manifest_reply_t;
```

Total wire size: `9 + 2*N` bytes. For Phase 2h's `register_dongle` (m2s = REGISTER_ACK, PING, GET_MANIFEST, OPERATIONAL_BEGIN) that's 17 bytes.

**`schema_hash` is FNV-1a 32-bit over the literal string:**

```
manifest_v1:schema_hash:u32,firmware_version:u32,m2s_count:u8,m2s_ops:u16[]
```

Computed at firmware build time; baked as a compile-time constant. Host has the same constant in its decoder. Hash match = host knows the layout. Future schema additions bump the version prefix (`manifest_v2:...`) → new hash → host dictionary upgrade required.

**v1 deliberately excludes:**

- per-opcode flags (state-precondition, payload-shape) — host's schema dictionary holds these out-of-band, keyed by `schema_hash`
- s2m opcode list — dongle declares "what I accept"; host decodes-or-skips unknown s2m
- variable-length return shapes — deferred to app-shell work
- `adc_bits` / per-class metadata — `class_id` in `OP_REGISTER` already pins the class; lookup metadata via that

Add any of the above by bumping the schema version.

### 4.5 `OP_OPERATIONAL_BEGIN` (m2s)

Empty payload. Bare opcode = "L2 pull is done; you may now emit heartbeats." Configuration (heartbeat rate, debug verbosity, etc.) is a separate opcode if/when needed — not muddled into the state advance.

### 4.6 `OP_NAK` (s2m)

```c
typedef enum {
    NAK_ERR_STATE           = 1,  // opcode not legal in current dongle state
    NAK_ERR_UNSUPPORTED_CMD = 2,  // opcode unknown or not implemented (e.g. leaf got OP_SLAVE_REGISTER)
    NAK_ERR_NO_RESOURCES    = 3,  // bounded table full, RAM exhausted, etc.
    NAK_ERR_ARGS            = 4,  // payload parse error or argument out of range
} nak_reason_t;

typedef struct {
    uint8_t  reason_code;    //  1   nak_reason_t
    uint16_t rejected_cmd;   //  2   opcode that triggered the NAK
} op_nak_t;                  // = 3 bytes
```

### 4.7 L0 — `OP_COMMISSION_SET` (m2s)

```c
typedef struct {
    uint32_t new_instance_id;    //  4   non-zero
    // optional friendly_name field — open per §9
} op_commission_set_t;
```

Issued **only** by the standalone production commissioning tool (§6). Operational stacks never emit this.

### 4.8 L0 — `OP_COMMISSION_REPLY` (s2m)

```c
typedef struct {
    uint32_t stored_instance_id;  //  4   echo of what was just persisted
    uint8_t  status;              //  1   0=ok, 1=flash_write_failed, 2=bad_args
} op_commission_reply_t;          // = 5 bytes
```

### 4.9 L0 — `OP_COMMISSION_CLEAR` (m2s)

Empty payload. Factory-resets the commissioning page → reboot → uncommissioned. Two-step re-commissioning rule: never combine clear + set; each step is auditable.

### 4.10 `OP_HEARTBEAT` / `OP_PONG` / `OP_DBG_LOG` / `OP_PING`

Out of scope for this spec — defined under Phase 2d–2g (see `samd21/apps/register_dongle/main.c` and `user_functions.c`).

---

## 5. Locked protocol rules

| Rule | Why |
|---|---|
| **Host drives all progression** with explicit advance opcodes | Dongle never has to infer "is host done with this layer?" — every state change has a single causal message |
| **No `OP_MANIFEST_ACK`** | Manifest pull is read-only; `OP_OPERATIONAL_BEGIN` is the only advance from L1_DONE |
| **Generic `OP_NAK { reason_code, rejected_cmd }`** | One NAK opcode for all state/permission errors; reasons in §4.6 |
| **L0 rides existing `OP_REGISTER` envelope** | `commissioning_state` byte tells host whether to route to L0 (commission UI) or L1 (role-bind) handler — no separate L0-only registration opcode |
| **m2s opcodes in 0x0100+** | Avoids collision with `SE_EVENT_TICK=4` — verified the hard way (Phase 2d PONG-on-every-tick bug) |
| **One in-flight per direction** (libcomm baseline) | From `dongle_linux_protocol_2026-05-11` — applies to all opcodes here |

---

## 6. L0 commissioning — out of operational scope

`OP_COMMISSION_SET` / `OP_COMMISSION_CLEAR` / `OP_COMMISSION_REPLY` are issued **only** by a standalone production LuaJIT tool (location TBD, sibling to `linux/dongle_console/`).

Operational Linux stacks — `mqtt_robot`, `fleet_manager`, `fake_robot` (per `fleet_design/`), future Linux robot containers — **must not**:

- emit `OP_COMMISSION_*` opcodes
- depend on commissioning happening at runtime
- host a commissioning UI

An operational stack that discovers an uncommissioned dongle (REGISTER with `commissioning_state = 0` or `instance_id = 0`) logs the fact and ignores the dongle. No auto-commissioning, no fallback.

Rationale: separation of concerns. Commissioning is a deliberate human bench operation; mixing it into the operational stack risks accidental re-commissioning during recovery / version skew / test escape. See `usb_commissioning_tool_scope_2026-05-19` memory for full rationale.

---

## 7. Initial sync — wire trace (worked example)

Boot of a commissioned `samd21_shell_v1` dongle, host running `dongle_console`:

```
T+0    s2m  OP_REGISTER          version=2 class_id=CLASS_SAMD21_SHELL_V1 instance_id=17
                                 commissioning_state=1 chip_uid=... vid=2886 pid=802f fw=...
T+1s   s2m  OP_REGISTER          (retry — host not yet ready)
T+2s   s2m  OP_REGISTER          (retry)
T+2.1s m2s  OP_REGISTER_ACK      → dongle: BOOT → L1_DONE
T+2.2s m2s  OP_GET_MANIFEST
T+2.2s s2m  OP_MANIFEST_REPLY    schema_hash=0xDEADBEEF fw=0x00010003
                                 m2s_count=4 ops={0x0103,0x0104,0x0107,0x0108}
T+2.3s m2s  OP_OPERATIONAL_BEGIN → dongle: L1_DONE → OPERATIONAL
T+3.3s s2m  OP_HEARTBEAT         seq=0 uptime_ms=3300
T+4.3s s2m  OP_HEARTBEAT         seq=1 uptime_ms=4300
...
```

Out-of-state opcode example:

```
T+5s   m2s  OP_PING              (legal in OPERATIONAL)
T+5s   s2m  OP_PONG              seq=0
T+6s   m2s  OP_REGISTER_ACK      (illegal — already past L1)
T+6s   s2m  OP_NAK               reason=err_state rejected_cmd=0x0103
```

---

## 8. Implementation map for SAMD21 firmware

Phase 2h step list, in order. Each row is implementation work that follows this spec.

| # | File / change | Notes |
|---|---|---|
| 1 | `samd21/apps/register_dongle/vendor/libcomm/opcodes.h` — add the four new opcodes per §3.2 + the L0 trio per §4.7–§4.9 | Resolve §3.3 conflict first |
| 2 | `samd21/apps/register_dongle/main.c` — RX dispatch handles `OP_GET_MANIFEST`, `OP_OPERATIONAL_BEGIN`, and emits `OP_NAK` for out-of-state | Pattern follows existing `OP_PING` → engine event_queue |
| 3 | `register_dongle_v2.lua` — extend `state_machine` with `L1_DONE` and `OPERATIONAL` cases; heartbeat fork lives only in `OPERATIONAL` case | Per `s_engine_dsl_composition_rules` |
| 4 | New user fn `send_manifest_reply` (s2m emit) and `send_nak` (s2m emit) | Pattern follows existing `send_register` / `send_pong` |
| 5 | Compile-time `schema_hash` constant — small Lua helper or C macro that hashes the schema-def string at build | FNV-1a function already exists in `s_engine_types.h::s_expr_hash` |
| 6 | `linux/dongle_console/dongle_console.lua` — add `--sync` flag that walks REGISTER_ACK → GET_MANIFEST → OPERATIONAL_BEGIN | Replaces current single-step `--send-ack` |
| 7 | Verify on real Xiao SAMD21 — full sync ladder + heartbeat starts only after `OP_OPERATIONAL_BEGIN` | Live trace per §7 |

L0 commissioning work (flash storage, `OP_COMMISSION_*` handlers, standalone tool) is a follow-on milestone — not blocking steps 1–7. See `dongle_class_identity_2026-05-13` for the L0 firmware-side details.

---

## 9. Open items

These need dialog before / during firmware work — flagged here so they're not lost.

1. **`build_date` redundancy with `fw_version`** in `OP_REGISTER` v2 payload (§4.1) — pick one or document why both.
2. **`friendly_name` in `OP_COMMISSION_SET`** (§4.7) — yes/no; if yes, length cap (recommended 32 bytes).
3. **Commissioning authentication** — `OP_COMMISSION_SET` is unsigned in the bench-friendly default. Flag for security review when leaving bench scope.
4. **Manifest v2 trigger** — what addition would justify bumping past v1? (per-opcode flags is the most likely.)
5. **`OP_GET_MANIFEST` rate-limiting** — host can refresh in OPERATIONAL; should dongle throttle? Probably not needed at libcomm's one-in-flight cadence.

---

## 10. Cross-references

| Topic | Where |
|---|---|
| Four-layer semantic model (universal, both directions) | `~/.claude/projects/-home-gedgar-motioncore-prototype/memory/four_layer_protocol_2026-05-16.md` |
| RS-485 slave wire (slave↔router-dongle, L1/L2 equivalent on RS-485) | `~/.claude/projects/.../rs485_slave_protocol_2026-05-16.md` |
| REGISTER v2 payload schema + flash storage per chip | `~/.claude/projects/.../dongle_class_identity_2026-05-13.md` |
| libcomm framing (SLIP + CRC-8/AUTOSAR), recovery handshake | `~/.claude/projects/.../dongle_linux_protocol_2026-05-11.md` |
| s_engine DSL composition rules (chain semantics for state machine) | `~/.claude/projects/.../s_engine_dsl_composition_rules.md` |
| USB commissioning tool — scope boundary | `~/.claude/projects/.../usb_commissioning_tool_scope_2026-05-19.md` |
| Current SAMD21 firmware | `motioncore-prototype/samd21/apps/register_dongle/` |
| Current host-side debug tool | `motioncore-prototype/linux/dongle_console/dongle_console.lua` (+ Pi copy at `/home/pi/dongle_console/`) |
| Top-level project plan + Phase 2h context | `motioncore-prototype/continue.md` ("2026-05-16 design dialog" section onward) |
