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
#define INTERLOCK_PERSIST_VERSION    1u

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
    uint32_t last_pc;            // PC of faulting instruction, 0 if none
    uint32_t last_lr;            // LR at fault entry
    uint32_t last_rstsr;         // PM->RCAUSE snapshot at the time of fault
    uint8_t  last_crashed_slot;  // slot index active during last fault, 0xFF if N/A
    uint8_t  reserved[3];
} interlock_crash_record_t;

typedef struct {
    uint32_t                 magic;
    uint8_t                  version;
    uint8_t                  reserved[3];
    interlock_slot_persist_t slots[INTERLOCK_MAX_SLOTS];
    interlock_crash_record_t crash;
} interlock_persist_t;

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

// Mark slot EMPTY. No-op if already EMPTY.
uint8_t  interlock_disarm_slot(uint8_t slot);

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
