// ============================================================================
// bus_roster.h — RAM-only RS-485 slave roster (bus_controller, Stage 2).
//
// See bus_roster.c and docs/rs485-bus-protocol-bc2-bc3.md §5.
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define BUS_ROSTER_MAX 16u   // bounded; a real bench bus is small (addr space 254)

// Per-slave state (filled by the Stage-3 poll engine; UNKNOWN until first poll).
#define BUS_STATE_UNKNOWN 0u
#define BUS_STATE_ALIVE   1u
#define BUS_STATE_DEAD    2u

// Per-slave flags.
#define BUS_FLAG_TCP      0x01u   // bit0: command this slave over TCP (else UDP)
#define BUS_FLAG_ENABLED  0x02u   // bit1: enabled (polled)

typedef struct {
    uint8_t  addr;                 // 1..254; 0 marks an empty slot
    uint32_t class_id;             // Pi bookkeeping + optional REGISTER sanity check
    uint8_t  flags;                // BUS_FLAG_*
    uint8_t  state;                // BUS_STATE_*
    uint8_t  consecutive_misses;
    uint32_t last_seen_ms;         // board_millis() of last good frame (0 = never)
} bus_slave_t;

// Poll config — stored in Stage 2, consumed by the Stage-3 poll engine.
typedef struct {
    uint16_t poll_period_ms;
    uint8_t  max_misses;
    uint8_t  tcp_retries;
    uint8_t  enabled;              // master poll enable (CMD_BUS_POLL_ENABLE)
} bus_poll_cfg_t;

// bus_roster_register result codes (also the CMD_BUS_REGISTER_SLAVE reply body).
#define BUS_REG_OK      0u
#define BUS_REG_FULL    1u   // roster at BUS_ROSTER_MAX
#define BUS_REG_DUP     2u   // addr already registered
#define BUS_REG_BADADDR 3u   // addr 0 or 0xFF

void bus_roster_init(void);

// Returns BUS_REG_*. On any outcome, *out_count (if non-NULL) gets the live
// roster count. OK inserts; DUP/FULL/BADADDR leave the roster unchanged.
uint8_t bus_roster_register(uint8_t addr, uint32_t class_id, uint8_t flags,
                            uint8_t* out_count);

// Returns true if a slot was freed. *out_count gets the resulting count.
bool bus_roster_unregister(uint8_t addr, uint8_t* out_count);

void               bus_roster_clear(void);
uint8_t            bus_roster_count(void);
const bus_slave_t* bus_roster_at(uint8_t index);   // 0..count-1 over occupied slots
bus_slave_t*       bus_roster_find(uint8_t addr);   // NULL if absent

bus_poll_cfg_t*    bus_poll_cfg(void);
