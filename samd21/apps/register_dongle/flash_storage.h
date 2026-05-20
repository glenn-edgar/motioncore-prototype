// ============================================================================
// flash_storage.h — persistent commissioning blob on SAMD21 NVMCTRL.
//
// Stores { instance_id, commissioning_state } durably across reboots in two
// flash rows at the top of the chip's 256 KB flash. Dual-slot rotation with
// a magic+sequence pattern gives power-loss tolerance: at worst we lose the
// most-recent commission, never corrupt to garbage.
//
// Layout per slot (1 row = 256 B; only first ~16 B used, rest erased 0xFF):
//   [0..3]   magic           = 0xC0FFEE00 when valid, 0xFFFFFFFF when erased
//   [4..7]   sequence        monotonic; higher wins on read tie
//   [8..11]  instance_id     0 if uncommissioned
//   [12]     commissioning_state  0=UNCOMMISSIONED, 1=COMMISSIONED
//   [13..15] padding
//
// Slot addresses (last 2 rows of 256 KB flash):
//   Slot A: 0x3FE00
//   Slot B: 0x3FF00
// Constraint: firmware text+data must end below 0x3FE00 (245 KB headroom
// from app origin 0x2000). Current build uses ~31 KB → ample margin.
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define COMMISSIONING_UNCOMMISSIONED  0u
#define COMMISSIONING_COMMISSIONED    1u

typedef struct {
    uint32_t instance_id;
    uint8_t  commissioning_state;
} commission_blob_t;

// Read the latest valid commissioning blob from flash. Returns true and
// populates *out if a valid slot exists; returns false on factory-fresh
// hardware (both slots erased) and leaves *out untouched.
bool flash_storage_read(commission_blob_t* out);

// Atomically write a new commissioning blob. Picks the inactive slot, erases
// it, writes the new data with sequence incremented, verifies. Returns true
// on success.
bool flash_storage_write(uint32_t instance_id, uint8_t commissioning_state);
