// ============================================================================
// shell_commands.c — general-layer implementation: cursor primitives,
// dispatch table, and CMD_ECHO seed command.
// ============================================================================

#include "shell_commands.h"
#include "vendor/libcomm/opcodes.h"   // SHELL_STATUS_* enum
#include <string.h>

// ---------- reader --------------------------------------------------------

void sr_init(shell_reader_t* r, const uint8_t* buf, uint16_t len) {
    r->start    = buf;
    r->p        = buf;
    r->end      = buf + len;
    r->overflow = false;
}

uint8_t sr_u8(shell_reader_t* r) {
    if (r->p + 1 > r->end) { r->overflow = true; return 0; }
    return *r->p++;
}

uint16_t sr_u16(shell_reader_t* r) {
    if (r->p + 2 > r->end) { r->overflow = true; return 0; }
    uint16_t v = (uint16_t)r->p[0] | ((uint16_t)r->p[1] << 8);
    r->p += 2;
    return v;
}

uint32_t sr_u32(shell_reader_t* r) {
    if (r->p + 4 > r->end) { r->overflow = true; return 0; }
    uint32_t v = (uint32_t)r->p[0]
               | ((uint32_t)r->p[1] <<  8)
               | ((uint32_t)r->p[2] << 16)
               | ((uint32_t)r->p[3] << 24);
    r->p += 4;
    return v;
}

void sr_bytes(shell_reader_t* r, uint8_t* out, uint16_t n) {
    if (r->p + n > r->end) { r->overflow = true; return; }
    memcpy(out, r->p, n);
    r->p += n;
}

uint16_t sr_remaining(const shell_reader_t* r) {
    return (uint16_t)(r->end - r->p);
}

// ---------- writer --------------------------------------------------------

void sw_init(shell_writer_t* w, uint8_t* buf, uint16_t cap) {
    w->start    = buf;
    w->p        = buf;
    w->end      = buf + cap;
    w->overflow = false;
}

void sw_u8(shell_writer_t* w, uint8_t v) {
    if (w->p + 1 > w->end) { w->overflow = true; return; }
    *w->p++ = v;
}

void sw_u16(shell_writer_t* w, uint16_t v) {
    if (w->p + 2 > w->end) { w->overflow = true; return; }
    w->p[0] = (uint8_t)(v & 0xFFu);
    w->p[1] = (uint8_t)(v >> 8);
    w->p += 2;
}

void sw_u32(shell_writer_t* w, uint32_t v) {
    if (w->p + 4 > w->end) { w->overflow = true; return; }
    w->p[0] = (uint8_t)(v >>  0);
    w->p[1] = (uint8_t)(v >>  8);
    w->p[2] = (uint8_t)(v >> 16);
    w->p[3] = (uint8_t)(v >> 24);
    w->p += 4;
}

void sw_bytes(shell_writer_t* w, const uint8_t* in, uint16_t n) {
    if (w->p + n > w->end) { w->overflow = true; return; }
    memcpy(w->p, in, n);
    w->p += n;
}

uint16_t sw_len(const shell_writer_t* w) {
    return (uint16_t)(w->p - w->start);
}

// ---------- CMD_ECHO ------------------------------------------------------
// Copy args verbatim into result. Validates the round-trip without depending
// on any chip-specific functionality.
//
// args_message:   len:u16  bytes:u8[len]
// result_message: len:u16  bytes:u8[len]   // bytes byte-for-byte copy

#define CMD_ECHO_MAX_LEN 64u

static uint8_t cmd_echo(shell_reader_t* args, shell_writer_t* result) {
    uint16_t len = sr_u16(args);
    if (args->overflow)        return SHELL_STATUS_BAD_ARGS;
    if (len > CMD_ECHO_MAX_LEN) return SHELL_STATUS_BAD_ARGS;

    uint8_t buf[CMD_ECHO_MAX_LEN];
    sr_bytes(args, buf, len);
    if (args->overflow) return SHELL_STATUS_BAD_ARGS;

    sw_u16  (result, len);
    sw_bytes(result, buf, len);
    if (result->overflow) return SHELL_STATUS_RESULT_TOO_BIG;

    return SHELL_STATUS_OK;
}

// ---------- CMD_SYSINFO ---------------------------------------------------
// Dump chip memory layout + runtime state. Args empty; result is a 37-byte
// binary message (see firmware_sysinfo_t docs in shell_commands.h).

static uint8_t cmd_sysinfo(shell_reader_t* args, shell_writer_t* result) {
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;

    firmware_sysinfo_t info;
    firmware_get_sysinfo(&info);

    sw_u8 (result, 1);                          // version
    sw_u16(result, info.flash_total_kb);
    sw_u32(result, info.flash_text_b);
    sw_u32(result, info.flash_data_b);
    sw_u16(result, info.ram_total_kb);
    sw_u32(result, info.ram_bss_b);
    sw_u32(result, info.ram_stack_b);
    sw_u32(result, info.bump_capacity_b);
    sw_u32(result, info.bump_peak_b);
    sw_u32(result, info.uptime_ms);
    sw_u32(result, info.cpu_clock_hz);

    if (result->overflow) return SHELL_STATUS_RESULT_TOO_BIG;
    return SHELL_STATUS_OK;
}

// ---------- dispatch table ------------------------------------------------

const shell_cmd_entry_t g_shell_cmds[] = {
    { CMD_ECHO,    "echo",    cmd_echo    },
    { CMD_SYSINFO, "sysinfo", cmd_sysinfo },
};

const uint8_t g_shell_cmd_count = sizeof(g_shell_cmds) / sizeof(g_shell_cmds[0]);

const shell_cmd_entry_t* shell_find_cmd(uint16_t command_id) {
    // General layer first.
    for (uint8_t i = 0; i < g_shell_cmd_count; i++) {
        if (g_shell_cmds[i].command_id == command_id) {
            return &g_shell_cmds[i];
        }
    }
    // Fall through to chip-specific commands.
    const shell_cmd_entry_t* chip = chip_commands_table();
    uint8_t                  n    = chip_commands_count();
    for (uint8_t i = 0; i < n; i++) {
        if (chip[i].command_id == command_id) {
            return &chip[i];
        }
    }
    return NULL;
}
