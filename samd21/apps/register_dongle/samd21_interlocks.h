// ============================================================================
// samd21_interlocks.h — interlock framework foundation (slice 1).
//
// Slice 1 ships ONLY the persistence + boot-decision + crash-context
// machinery, not the DSL parser, voting, or status emission. A hardcoded
// "no-op" interlock is provided so slot lifecycle can be exercised
// end-to-end via CMD_INTERLOCK_ARM_NOOP + CMD_TEST_HANG. Later slices add:
//   - slice 2: text DSL parser (gpio_int, adc_int) + CMD_SET_INTERLOCK
//   - slice 3: HAL pin-claim API + output OR-of-vetoes voting
//   - slice 4: 64-B status buffer + OP_POLL / OP_EVENT wire protocol
//
// Design lives in:
//   memory/samd21_interlock_framework_design.md  (private)
//   docs/interlock-framework-prior-art.md         (public, MIT)
//
// Persistence note: g_interlock_persist sits in a linker section named
// ".noinit" which must be marked NOLOAD and excluded from .bss zeroing in
// seeeduino_xiao.ld. WDT / software / external resets preserve RAM
// contents; POR + brown-out wipe and re-initialise via magic mismatch.
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

// ---- Constants ------------------------------------------------------------

#define INTERLOCK_MAGIC              0xCD51AC73u
#define INTERLOCK_MAX_SLOTS          2u
#define INTERLOCK_MAX_BOOT_ATTEMPTS  3u
#define INTERLOCK_ID_NONE            0u
#define INTERLOCK_ID_NOOP            1u
#define INTERLOCK_CRASHED_SLOT_NONE  0xFFu

// Version field in interlock_persist_t — bump when struct layout changes so
// future-firmware boots detect old-firmware noinit data and re-initialise.
// v1 (slice 1): magic + per-slot state/id/boot_counter + crash record only.
// v2 (slice 2): adds il_inst_t + dsl_text per slot.
// v3 (slice 4): grows il_input_t with oversample_exp + sh_cyc for ADC inputs.
// v4 (slice 5): adds self_size header + panic_code/panic_arg/panic_sp to crash record.
// v5 (slice 6): grows il_input_t to 8 B with debounce_depth + hyst for ADC/virtual.
#define INTERLOCK_PERSIST_VERSION    5u

// ---- Slice 2: DSL-driven interlock instance ------------------------------

#define IL_NAME_MAX        16u
#define IL_DSL_MAX        128u
#define IL_MAX_INPUTS       4u
#define IL_MAX_WATCHES      4u
#define IL_MAX_OUTPUTS      2u

// id=2 means "this slot's behaviour is described by il_inst_t / dsl_text"
// (as opposed to id=1 = hardcoded noop). Wire-format compatibility with v1.
#define INTERLOCK_ID_DSL    2u

typedef enum {
    IL_PIN_MODE_IN     = 0,
    IL_PIN_MODE_IN_PU  = 1,
    IL_PIN_MODE_IN_PD  = 2,
    IL_PIN_MODE_OUT    = 3,
    IL_PIN_MODE_ADC    = 4,   // slice 4: 12-bit analog input
    IL_PIN_MODE_VIRTUAL = 5,  // slice 6: synthesized input (no physical claim)
} il_pin_mode_t;

// Virtual-input IDs. When il_input_t.mode == IL_PIN_MODE_VIRTUAL, phys_id
// names which synthesized signal feeds the watch. Values >= 0xF0 keep them
// disjoint from real phys_ids (board_pin_phys_id() max is 0x3F = (1<<5)|31).
#define IL_VIRT_T_SINCE_M2S  0xF0u   // seconds since last received m2s frame
#define IL_VIRT_UPTIME       0xF1u   // seconds since boot
#define IL_VIRT_STACK_HWM    0xF2u   // peak stack depth in bytes

typedef enum {
    IL_OP_EQ = 0,
    IL_OP_NE = 1,
    IL_OP_LT = 2,
    IL_OP_GT = 3,
    IL_OP_LE = 4,
    IL_OP_GE = 5,
} il_compare_op_t;

typedef enum {
    IL_TF_UNEVALUATED = 0,
    IL_TF_TRUE        = 1,   // all watch clauses satisfied → vote OK
    IL_TF_FALSE       = 2,   // any clause failed → vote ERR
} il_tf_state_t;

typedef struct {
    uint8_t  phys_id;             // resolved board_pin_phys_id() OR IL_VIRT_* when mode=VIRTUAL
    uint8_t  mode;                // il_pin_mode_t
    uint8_t  oversample_exp;      // ADC mode only (0..4); ignored for GPIO/VIRTUAL
    uint8_t  sh_cyc;              // ADC mode only (0..63); ignored for GPIO/VIRTUAL
    uint8_t  debounce_depth;      // slice 6: 0 = none, 2..15 = shift-register depth; GPIO inputs only
    uint8_t  reserved;
    uint16_t hyst;                // slice 6: 0 = none, dead-band around gt/lt/ge/le; ADC only
} il_input_t;

typedef struct {
    uint8_t  input_idx;           // index into il_inst_t.inputs[]
    uint8_t  op;                  // il_compare_op_t
    uint16_t threshold;           // raw value (0/1 for GPIO; 0..4095 for ADC)
} il_watch_t;

typedef struct {
    uint8_t  phys_id;
    uint8_t  ok_value;            // 0 or 1
    uint8_t  err_value;           // 0 or 1
} il_output_t;

typedef struct {
    char        name[IL_NAME_MAX];
    uint8_t     input_count;
    uint8_t     watch_count;
    uint8_t     output_count;
    uint8_t     tf_state;         // il_tf_state_t — current evaluation result
    il_input_t  inputs[IL_MAX_INPUTS];
    il_watch_t  watches[IL_MAX_WATCHES];
    il_output_t outputs[IL_MAX_OUTPUTS];
} il_inst_t;

// ---- Parser status codes -------------------------------------------------

typedef enum {
    IL_PARSE_OK                    = 0,
    IL_PARSE_UNEXPECTED_CHAR       = 1,
    IL_PARSE_UNEXPECTED_END        = 2,
    IL_PARSE_BAD_NUMBER            = 3,
    IL_PARSE_UNKNOWN_KEYWORD       = 4,
    IL_PARSE_UNKNOWN_PIN           = 5,
    IL_PARSE_UNKNOWN_MODE          = 6,
    IL_PARSE_TOO_MANY_INPUTS       = 7,
    IL_PARSE_TOO_MANY_WATCHES      = 8,
    IL_PARSE_TOO_MANY_OUTPUTS      = 9,
    IL_PARSE_NAME_TOO_LONG         = 10,
    IL_PARSE_DUPLICATE_PIN         = 11,
    IL_PARSE_WATCH_INPUT_UNDECL    = 12,  // watch references a pin not in cfg
    IL_PARSE_OUTPUT_UNDECL         = 13,  // out_ok/err references undeclared pin
    IL_PARSE_OUTPUT_VALUE_MISMATCH = 14,  // out_ok and out_err disagree on pin set
    IL_PARSE_MISSING_OUT_OK        = 15,
    IL_PARSE_MISSING_OUT_ERR       = 16,
    IL_PARSE_EMPTY                 = 17,
    // Slice 4 — ADC + comparison-op extensions
    IL_PARSE_UNKNOWN_OP             = 18,  // watch op not in {eq,ne,lt,gt,le,ge}
    IL_PARSE_OVERSAMPLE_OUT_OF_RANGE = 19, // oversample_N: N not in {1,2,4,8,16}
    IL_PARSE_SH_OUT_OF_RANGE        = 20,  // sh_N: N > 63
    IL_PARSE_MODIFIER_ON_GPIO       = 21,  // oversample_N or sh_N on non-adc cfg
    IL_PARSE_THRESHOLD_OUT_OF_RANGE = 22,  // watch threshold > 65535 (defence — read_number truncates)
    // Slice 6 — hysteresis + debounce + virtual inputs
    IL_PARSE_HYST_NOT_ON_ADC        = 23,  // hyst_N requires :adc mode
    IL_PARSE_DEBOUNCE_NOT_ON_GPIO   = 24,  // debounce_N requires :in/up/down mode
    IL_PARSE_DEBOUNCE_OUT_OF_RANGE  = 25,  // debounce_N: N not in [2,15]
    IL_PARSE_UNKNOWN_VIRTUAL        = 26,  // _name not in virtual-input table
    IL_PARSE_HYST_ON_EQ_NE          = 27,  // watch op eq/ne against a pin with hyst configured
} il_parse_status_t;

// Parse DSL text into out. text is not required to be NUL-terminated.
// On success returns IL_PARSE_OK and populates *out. On failure, returns
// the error category and (if err_offset non-NULL) the byte offset within
// text where the error was detected. *out is left in undefined state.
il_parse_status_t il_parse(const char* text, uint16_t text_len,
                           il_inst_t* out, uint16_t* err_offset);

// ---- Types ---------------------------------------------------------------

typedef enum {
    INTERLOCK_SLOT_EMPTY    = 0,
    INTERLOCK_SLOT_ARMED    = 1,
    INTERLOCK_SLOT_POISONED = 2,
} interlock_slot_state_t;

typedef struct {
    uint8_t  state;          // interlock_slot_state_t
    uint8_t  id;             // 1-based index into g_interlocks[]; 0 = none
    uint8_t  boot_counter;   // warm boots observed since this slot was armed
    uint8_t  reserved;
} interlock_slot_persist_t;

typedef struct {
    uint32_t last_pc;            // PC of faulting instruction (HardFault) or
                                 // panic() return-address. 0 if none.
    uint32_t last_lr;            // LR at fault entry (HardFault only; 0 for panic)
    uint32_t last_rstsr;         // PM->RCAUSE snapshot at the time of fault/panic
    uint8_t  last_crashed_slot;  // slot index active during last fault, 0xFF if N/A
    uint8_t  panic_code;         // panic_code_t value; 0 = no panic (HardFault path or fresh)
    uint8_t  reserved[2];
    uint32_t panic_arg;          // panic()'s second argument (e.g., bad-magic value, SP)
    uint32_t panic_sp;           // SP at panic() entry — useful for stack-near-overflow
} interlock_crash_record_t;

typedef struct {
    uint32_t                 magic;
    uint8_t                  version;
    uint8_t                  reserved;        // pad to align self_size at offset 6
    uint16_t                 self_size;       // sizeof(interlock_persist_t); 0 in v<=3
    interlock_slot_persist_t slots[INTERLOCK_MAX_SLOTS];
    interlock_crash_record_t crash;
    // Slice 2 additions — present only when version >= 2.
    il_inst_t                inst[INTERLOCK_MAX_SLOTS];
    uint16_t                 dsl_len[INTERLOCK_MAX_SLOTS];
    char                     dsl_text[INTERLOCK_MAX_SLOTS][IL_DSL_MAX];
} interlock_persist_t;

// ---- Panic codes (slice 5, Amendment C) ----------------------------------
// Software-detected invariant violations. HardFault_Handler still uses its
// own record path (last_pc/lr from exception frame) and writes panic_code=0.
typedef enum {
    PANIC_NONE                  = 0,
    PANIC_STACK_NEAR_OVERFLOW   = 1,  // SP dropped below _sstack + margin
    PANIC_PERSIST_MAGIC_BAD     = 2,  // g_interlock_persist.magic mismatched mid-run
    PANIC_PERSIST_VERSION_BAD   = 3,  // version mismatched mid-run
    PANIC_PERSIST_SIZE_BAD      = 4,  // self_size mismatched mid-run
    PANIC_HAL_PIN_DUPLICATE     = 5,  // pin-claim table has duplicate non-shared entry
    PANIC_INIT_CANARY_BAD       = 6,  // stack canary failed at end of init
    PANIC_CRASH_RECORD_BAD      = 7,  // crash record self-inconsistent at boot
    /* extend per slice */
} panic_code_t;

// Non-returning. Records code/arg/SP/uptime into the crash slot and resets
// the chip. Use only for software-detected invariants — HardFault has its
// own path.
__attribute__((noreturn))
void panic(panic_code_t code, uint32_t arg);

// ---- Status buffer (slice 5, main work) ----------------------------------
// 64 B compact snapshot built by interlock_tick_all() at the end of each
// tick. Optimised for high-frequency host polling (replaces the on-demand
// 55 B CMD_INTERLOCK_STATUS assembly path).
//
// Wire format: this struct is copied verbatim into an OP_POLL_REPLY frame
// payload. All multi-byte fields are little-endian (Cortex-M0+ native).
//
// Compatibility: bump IL_STATUS_BUFFER_VERSION on any layout change. Host
// decoder reads version first and refuses unknown.
#define IL_STATUS_BUFFER_VERSION   1u
#define IL_STATUS_BUFFER_SIZE      64u
#define IL_STATUS_SLOT_NAME_MAX    8u    // truncated from IL_NAME_MAX=16

typedef struct __attribute__((packed)) {
    uint8_t  state;                          // interlock_slot_state_t
    uint8_t  id;                             // INTERLOCK_ID_* value
    uint8_t  tf;                             // il_tf_state_t
    uint8_t  bc;                             // boot_counter
    char     name[IL_STATUS_SLOT_NAME_MAX];  // first 8 chars of inst.name
    uint8_t  veto_mask;                      // OR-of-vetoes contribution
    uint8_t  reserved;
} il_status_slot_t;   // 14 B

typedef struct __attribute__((packed)) {
    uint8_t  version;            // IL_STATUS_BUFFER_VERSION
    uint8_t  num_slots;          // INTERLOCK_MAX_SLOTS
    uint16_t status_seq;         // monotonic; bumps on any slot state/tf change
    uint8_t  crash_panic_code;   // last recorded panic code (0 = none)
    uint8_t  reserved0;
    uint16_t stack_hwm_bytes;    // peak observed stack depth
    il_status_slot_t slots[INTERLOCK_MAX_SLOTS];   // 14 × N bytes
    uint16_t input_vals[INTERLOCK_MAX_SLOTS][IL_MAX_INPUTS];  // raw readings
    uint8_t  reserved1[12];      // padding to 64 B
} il_status_buffer_t;

_Static_assert(sizeof(il_status_buffer_t) == IL_STATUS_BUFFER_SIZE,
               "il_status_buffer_t layout drift");

// Lives in .data (not .noinit — rebuilt fresh every tick; no persistence needed).
extern il_status_buffer_t g_il_status_buffer;

// Read-only accessor — host-side OP_POLL handler copies this into the reply.
const il_status_buffer_t* interlock_get_status_buffer(void);

// Compile-time registry entry. Slice 1 only uses .name; tick / init /
// terminate are placeholders for slice 2+.
typedef void (*interlock_fn_t)(void);
typedef struct {
    const char*    name;
    interlock_fn_t init;
    interlock_fn_t tick;
    interlock_fn_t terminate;
} interlock_def_t;

// ---- Globals -------------------------------------------------------------

// Persistent state — defined in samd21_interlocks.c with .noinit section
// attribute. Survives WDT / SW / EXT resets; wiped on POR.
extern interlock_persist_t g_interlock_persist;

// Compile-time interlock registry. Slot id = (index in array) + 1; id 0 is
// reserved for "no interlock."
extern const interlock_def_t g_interlocks[];
extern const uint8_t          g_interlock_count;

// Tracks which slot's code is currently executing. Updated by the (future)
// tick loop; HardFault_Handler reads it to record which slot crashed.
// Slice 1: stays at 0xFF (no tick loop yet).
extern volatile uint8_t       g_active_interlock_slot;

// ---- Boot lifecycle ------------------------------------------------------

// Run very early in main() — AFTER hal_capture_reset_cause(), BEFORE board /
// peripherals / engine init. Validates magic, decides cold vs warm boot,
// applies bootloop guard, and may mark slots POISONED.
//
// Returns reset cause bits for caller (bit set per slot that was warm-restored).
void     interlock_boot_decide(void);

// Number of slots currently ARMED. Useful in the boot-emit path.
uint8_t  interlock_armed_count(void);

// ---- Slot administration (slice 1 stubs) ---------------------------------

// Returns SHELL_STATUS_OK on success, *_BAD_ARGS on out-of-range slot,
// *_BUSY if the slot is already ARMED. Slice 1 only supports id =
// INTERLOCK_ID_NOOP; later slices accept DSL configurations.
uint8_t  interlock_arm_slot_noop(uint8_t slot);

// Mark slot EMPTY. No-op if already EMPTY. Releases all HAL pin claims
// belonging to this slot.
uint8_t  interlock_disarm_slot(uint8_t slot);

// Set a DSL-defined interlock into a slot. Steps performed atomically:
//   1. Parse text → temp il_inst_t
//   2. Claim every declared pin via the HAL pin API (rolls back on any failure)
//   3. Copy temp il_inst_t + text into the .noinit slot, mark ARMED (id=DSL)
// Returns SHELL_STATUS_* with detailed error categories on failure.
// `text` is the raw DSL bytes; not required to be NUL-terminated.
// `err_payload` (out, optional) gets a 3-byte error payload {category:u8,
// offset_lo:u8, offset_hi:u8} on parse failure; pass NULL to ignore.
uint8_t  interlock_set_slot_dsl(uint8_t slot,
                                const char* text, uint16_t text_len,
                                uint8_t err_payload[3]);

// Run AFTER samd21_peripherals_init() but before the first interlock_tick_all().
// Walks ARMED DSL slots, re-parses their persisted DSL text, re-claims pins
// via the HAL. Slots that fail re-parse / re-claim are marked POISONED.
// Idempotent for cold boots (no ARMED slots → nothing to do).
void     interlock_warm_restore(void);

// ---- Tick loop -----------------------------------------------------------

// Called once per chain pump (~250 ms) from main.c. Walks each ARMED slot,
// reads inputs, evaluates watches, drives outputs based on T/F result.
// Updates g_active_interlock_slot around each slot's tick so HardFault
// attribution is accurate.
void     interlock_tick_all(void);

// ---- Status access -------------------------------------------------------

// Read snapshot of slot N's persistent record. Returns NULL if slot >=
// INTERLOCK_MAX_SLOTS. Pointer remains valid (lives in .noinit).
const interlock_slot_persist_t* interlock_get_slot(uint8_t slot);

// Read snapshot of last crash record. Always non-NULL.
const interlock_crash_record_t* interlock_get_crash(void);

// ---- Boot diagnostics ----------------------------------------------------

// Render a one-line summary of the interlock state suitable for emission via
// debug_packet_fn / OP_DBG_LOG. Format (no trailing newline):
//
//   [BOOT_IL] sl0=S:I:C sl1=S:I:C pc=0xNNNNNNNN lr=0xNNNNNNNN rs=0xNNNNNNNN cs=N
//
// Where S = 'E'/'A'/'P', I = id, C = boot_counter, pc/lr/rs are 8-hex words
// from the crash record (all zero on cold boot), cs is the crashed slot
// index (0..N-1 or '-' for none). Returns characters written (excluding
// the NUL terminator). Pass a buffer ~96 bytes.
uint16_t interlock_format_boot_line(char* buf, uint16_t bufsize);

// ---- Fault handling ------------------------------------------------------

// C-callable from the HardFault_Handler naked thunk. Records crash context
// into g_interlock_persist.crash, then NVIC_SystemReset()s. Not for direct
// invocation from application code.
void     interlock_hardfault_record(uint32_t* msp_at_entry) __attribute__((noreturn));
