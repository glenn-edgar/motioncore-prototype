// ============================================================================
// flash_storage.h — persistent commissioning blob on the RA4M1 data flash.
//
// Stores { instance_id, commissioning_state } durably across reboots in the
// RA4M1's dedicated 8 KB data flash (memory-mapped at 0x40100000), which is a
// separate region from the 256 KB code flash. Because it is separate, the
// blob survives a code-flash reflash automatically — no linker carve-out,
// unlike the SAMD21 port which reserved the top two code-flash rows.
//
// Dual-slot rotation (magic + sequence) gives power-loss tolerance: re-
// commissioning triggers a deliberate reboot, so a write-then-power-event is
// in the normal path. Writing the *inactive* slot means a power loss mid-write
// corrupts at most the new slot — the committed identity in the old slot is
// never touched. The slot-selection logic is identical to the SAMD21 port;
// only the storage region and the erase/write primitives (FSP r_flash_lp)
// differ.
//
// Layout per slot (16 B; data flash is written in 4-byte units):
//   [0..3]   magic           = 0xC0FFEE00 when valid, 0xFFFFFFFF when erased
//   [4..7]   sequence        monotonic; higher wins on read tie
//   [8..11]  instance_id     0 if uncommissioned
//   [12]     commissioning_state  0=UNCOMMISSIONED, 1=COMMISSIONED
//   [13..15] padding
//
// Slot addresses — data-flash base + 2 KB spacing. 2 KB comfortably exceeds
// any plausible RA4M1 data-flash erase-block size, so erasing one slot's
// block can never disturb the other slot:
//   Slot A: 0x40100000
//   Slot B: 0x40100800
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

// Read the latest valid commissioning blob from data flash. Returns true and
// populates *out if a valid slot exists; returns false on factory-fresh
// hardware (both slots erased), and also false if the FSP flash driver fails
// to open — the RA4M1 data flash is only reliably readable after
// R_FLASH_LP_Open has configured the flash interface, so this opens it first.
bool flash_storage_read(commission_blob_t* out);

// Atomically write a new commissioning blob. Picks the inactive slot, erases
// it, writes the new data with sequence incremented, verifies. Opens the FSP
// flash driver lazily on first call. Returns true on success.
bool flash_storage_write(uint32_t instance_id, uint8_t commissioning_state);
