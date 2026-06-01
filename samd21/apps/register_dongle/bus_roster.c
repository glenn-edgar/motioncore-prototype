// ============================================================================
// bus_roster.c — RAM-only RS-485 slave roster for the bus_controller (Stage 2).
//
// The Pi is the single source of truth: it registers slaves at runtime via the
// CMD_BUS_* shell commands (see samd21_commands.c). On BC cold boot OR WDT
// recovery this roster is empty; the Pi re-registers after it sees the BC's
// OP_REGISTER. No flash, no second copy to keep in sync. Bus discovery (probe
// 1..254) is a standalone offline tool, never firmware — see the protocol spec
// docs/rs485-bus-protocol-bc2-bc3.md §9.
//
// Stage 2 builds ONLY the roster + the command surface. The autonomous poll
// engine that consumes g_poll_cfg ships in Stage 3.
// ============================================================================

#include "bus_roster.h"

// Bus management lives only on the bus_controller. On slave/dongle builds this
// is an empty translation unit (no roster BSS, no code).
#if defined(ROLE_BUS_CONTROLLER)

// Sparse table: addr == 0 marks an empty slot (0 is the master/broadcast addr,
// never a valid slave). Bounded at BUS_ROSTER_MAX — a real bench bus is small
// even though the 9-bit addr space is 254.
static bus_slave_t   g_roster[BUS_ROSTER_MAX];

// Poll config: defaults are conservative and overridden by CMD_BUS_SET_POLL /
// CMD_BUS_POLL_ENABLE before Stage 3's engine reads them.
static bus_poll_cfg_t g_poll_cfg;

void bus_roster_init(void) {
    for (uint8_t i = 0; i < BUS_ROSTER_MAX; i++) {
        g_roster[i].addr = 0;
    }
    g_poll_cfg.poll_period_ms = 100;   // 10 Hz round-robin default
    g_poll_cfg.max_misses     = 3;
    g_poll_cfg.tcp_retries    = 2;
    g_poll_cfg.enabled        = 0;     // polling off until the Pi enables it
}

bus_slave_t* bus_roster_find(uint8_t addr) {
    if (addr == 0) return 0;
    for (uint8_t i = 0; i < BUS_ROSTER_MAX; i++) {
        if (g_roster[i].addr == addr) return &g_roster[i];
    }
    return 0;
}

uint8_t bus_roster_count(void) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < BUS_ROSTER_MAX; i++) {
        if (g_roster[i].addr != 0) n++;
    }
    return n;
}

const bus_slave_t* bus_roster_at(uint8_t index) {
    uint8_t seen = 0;
    for (uint8_t i = 0; i < BUS_ROSTER_MAX; i++) {
        if (g_roster[i].addr == 0) continue;
        if (seen == index) return &g_roster[i];
        seen++;
    }
    return 0;
}

uint8_t bus_roster_register(uint8_t addr, uint32_t class_id, uint8_t flags,
                            uint8_t* out_count) {
    if (addr == 0 || addr == 0xFF) {   // 0 = master, 0xFF = sniffer/broadcast
        if (out_count) *out_count = bus_roster_count();
        return BUS_REG_BADADDR;
    }
    if (bus_roster_find(addr) != 0) {  // already registered — caller may re-set
        if (out_count) *out_count = bus_roster_count();
        return BUS_REG_DUP;
    }
    for (uint8_t i = 0; i < BUS_ROSTER_MAX; i++) {
        if (g_roster[i].addr != 0) continue;
        g_roster[i].addr               = addr;
        g_roster[i].class_id           = class_id;
        g_roster[i].flags              = flags;
        g_roster[i].state              = BUS_STATE_UNKNOWN;
        g_roster[i].consecutive_misses = 0;
        g_roster[i].last_seen_ms       = 0;
        g_roster[i].announced_state    = BUS_STATE_UNKNOWN;
        if (out_count) *out_count = bus_roster_count();
        return BUS_REG_OK;
    }
    if (out_count) *out_count = bus_roster_count();
    return BUS_REG_FULL;
}

bool bus_roster_unregister(uint8_t addr, uint8_t* out_count) {
    bus_slave_t* s = bus_roster_find(addr);
    if (s == 0) {
        if (out_count) *out_count = bus_roster_count();
        return false;
    }
    s->addr = 0;   // free the slot
    if (out_count) *out_count = bus_roster_count();
    return true;
}

void bus_roster_clear(void) {
    for (uint8_t i = 0; i < BUS_ROSTER_MAX; i++) {
        g_roster[i].addr = 0;
    }
}

const bus_slave_t* bus_roster_next_enabled(uint8_t* raw_cursor) {
    uint8_t start = (*raw_cursor) % BUS_ROSTER_MAX;
    for (uint8_t k = 0; k < BUS_ROSTER_MAX; k++) {
        uint8_t i = (uint8_t)((start + k) % BUS_ROSTER_MAX);
        const bus_slave_t* s = &g_roster[i];
        if (s->addr != 0 && (s->flags & BUS_FLAG_ENABLED)) {
            *raw_cursor = (uint8_t)((i + 1u) % BUS_ROSTER_MAX);   // resume past this one
            return s;
        }
    }
    return 0;   // no enabled slave
}

bus_poll_cfg_t* bus_poll_cfg(void) {
    return &g_poll_cfg;
}

#endif // ROLE_BUS_CONTROLLER
