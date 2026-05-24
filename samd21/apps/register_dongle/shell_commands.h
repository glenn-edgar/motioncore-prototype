// ============================================================================
// shell_commands.h — app-shell binary-message framing + command dispatch.
//
// Wire framing (libcomm-level) lives in vendor/libcomm/opcodes.h:
//   OP_SHELL_EXEC  m2s  [request_id u16][command_id u16][args_message bytes]
//   OP_SHELL_REPLY s2m  [request_id u16][status u8     ][result_message bytes]
//
// args_message and result_message are command-specific binary messages with
// little-endian primitives, parsed in order. Variable-length sections use the
// count-prefixed pattern (e.g., num_channels:u8 channels:u8[num_channels]).
//
// This file is the GENERAL layer: cursor readers/writers, the dispatch table,
// and the CMD_ECHO seed. Domain-specific commands (GPIO/ADC/PWM/quadrature)
// live in their own files and register themselves into g_shell_cmds[].
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

// ---------- binary-message cursor types -----------------------------------

typedef struct {
    const uint8_t* start;   // for sr_remaining()
    const uint8_t* p;       // read cursor
    const uint8_t* end;     // 1-past-end
    bool           overflow;
} shell_reader_t;

typedef struct {
    uint8_t*       start;   // for sw_len()
    uint8_t*       p;       // write cursor
    uint8_t*       end;     // 1-past-end (capacity boundary)
    bool           overflow;
} shell_writer_t;

void     sr_init     (shell_reader_t* r, const uint8_t* buf, uint16_t len);
uint8_t  sr_u8       (shell_reader_t* r);
uint16_t sr_u16      (shell_reader_t* r);
uint32_t sr_u32      (shell_reader_t* r);
void     sr_bytes    (shell_reader_t* r, uint8_t* out, uint16_t n);
uint16_t sr_remaining(const shell_reader_t* r);

void     sw_init     (shell_writer_t* w, uint8_t* buf, uint16_t cap);
void     sw_u8       (shell_writer_t* w, uint8_t  v);
void     sw_u16      (shell_writer_t* w, uint16_t v);
void     sw_u32      (shell_writer_t* w, uint32_t v);
void     sw_bytes    (shell_writer_t* w, const uint8_t* in, uint16_t n);
uint16_t sw_len      (const shell_writer_t* w);

// ---------- command dispatch ----------------------------------------------

// A command handler reads its args_message from *args, writes its
// result_message into *result, and returns a SHELL_STATUS_* value.
//
// Convention: if args parse fails (args->overflow becomes true), return
// SHELL_STATUS_BAD_ARGS. If the result writer overflows (would exceed
// libcomm payload), return SHELL_STATUS_RESULT_TOO_BIG. SHELL_STATUS_OK on
// success; other statuses for domain-specific failure modes.
typedef uint8_t (*shell_cmd_fn)(shell_reader_t* args, shell_writer_t* result);

typedef struct {
    uint16_t     command_id;
    const char*  name;        // for OP_DBG_LOG / debugging
    shell_cmd_fn fn;
} shell_cmd_entry_t;

extern const shell_cmd_entry_t g_shell_cmds[];
extern const uint8_t           g_shell_cmd_count;

// Returns NULL if command_id is not registered.
const shell_cmd_entry_t* shell_find_cmd(uint16_t command_id);

// ---------- general-layer command IDs -------------------------------------
// 0x0001..0x00FF reserved for the general layer (echo, ping, time, etc.).
// 0x0100+ for chip/role-specific commands (GPIO, ADC, PWM, quadrature, ...).

#define CMD_ECHO     ((uint16_t)0x0001)
#define CMD_SYSINFO  ((uint16_t)0x0002)

// ---------- chip-specific command IDs (0x0100..0x01FF: GPIO) --------------
// Pin coordinates on the wire are RAW (port:u8, pin:u8). Translation from
// board labels (e.g., Xiao "D2") to (port, pin) lives host-side — see
// linux/dongle_console/dongle_console.lua's resolve_pin().
//
// SAMD21 port:u8 — 0=PA, 1=PB. SAMD21G18A has groups PA (32 pins) + PB
// (32 pins, only some bonded out on the Xiao).

#define CMD_GPIO_CONFIG  ((uint16_t)0x0100)
#define CMD_GPIO_WRITE   ((uint16_t)0x0101)
#define CMD_GPIO_READ    ((uint16_t)0x0102)

// 0x0103..0x010F: DAC + ADC (SAMD21 functional HIL primitives).
#define CMD_DAC_WRITE         ((uint16_t)0x0103)
#define CMD_ADC_READ          ((uint16_t)0x0104)
#define CMD_DAC_WAVEFORM_WRITE ((uint16_t)0x0105)
#define CMD_DAC_STOP          ((uint16_t)0x0106)
#define CMD_ADC_CAPTURE       ((uint16_t)0x0107)

// 0x0108..0x010E: VACATED. Previously PWM (TCC0/WO0) and pulse counter
// (EIC→EVSYS→TC4 COUNT32). Removed when SAMD21 narrowed to safety/IO supervisor
// role — motor PWM is RA4M1 territory; pulse counting not part of the SAMD21
// supervisory contract. Opcode range left RESERVED-DO-NOT-REUSE so older
// dongle_console.lua copies will get SHELL_STATUS_UNKNOWN_CMD rather than
// silently re-dispatched to a different handler.

// 0x0120 — deliberate hang to verify layer-2 WDT recovery. Disables IRQs
// and spins forever. The WDT bites (~4 s) and the chip resets. Bench tool
// only; no reply frame is ever produced (the command never returns).
#define CMD_TEST_HANG         ((uint16_t)0x0120)

// GPIO mode codes for CMD_GPIO_CONFIG.
#define GPIO_MODE_INPUT          0u
#define GPIO_MODE_OUTPUT         1u
#define GPIO_MODE_INPUT_PULLUP   2u
#define GPIO_MODE_INPUT_PULLDOWN 3u

// ---------- chip-specific dispatch table (linker plugin point) ------------
// Each chip provides its own g_chip_commands[] via these two symbols.
// shell_find_cmd() searches g_shell_cmds[] (general) first, then chip table.
// A chip with no specific commands can return NULL + 0.

extern const shell_cmd_entry_t* chip_commands_table(void);
extern uint8_t                  chip_commands_count(void);

// ---------- sysinfo plumbing ----------------------------------------------
// CMD_SYSINFO returns a snapshot of the chip's memory layout + runtime state.
// firmware_get_sysinfo() is implemented per chip in the chip's main.c (or a
// chip-specific helper). All bytes are in u32; KB-scale totals in u16 to
// keep the wire payload small.
//
// result_message wire layout (37 bytes, version=1):
//   version:u8 = 1
//   flash_total_kb:u16  flash_text_b:u32  flash_data_b:u32
//   ram_total_kb:u16    ram_bss_b:u32     ram_stack_b:u32
//   bump_capacity_b:u32 bump_peak_b:u32
//   uptime_ms:u32       cpu_clock_hz:u32

typedef struct {
    uint16_t flash_total_kb;   // chip's total flash capacity
    uint32_t flash_text_b;     // code + rodata + .ARM.exidx (the linker's _etext - origin)
    uint32_t flash_data_b;     // .data initializer in flash (same size in RAM)
    uint16_t ram_total_kb;     // chip's total SRAM
    uint32_t ram_bss_b;        // .bss size in RAM (uninitialized)
    uint32_t ram_stack_b;      // stack reserved (linker STACK_SIZE)
    uint32_t bump_capacity_b;  // s_engine bump allocator buffer size
    uint32_t bump_peak_b;      // bump allocator peak usage observed
    uint32_t uptime_ms;        // monotonic since boot
    uint32_t cpu_clock_hz;     // SystemCoreClock (CMSIS) or equivalent
} firmware_sysinfo_t;

void firmware_get_sysinfo(firmware_sysinfo_t* out);
