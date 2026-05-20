// ============================================================================
// flash_storage.c — SAMD21 NVMCTRL dual-slot commissioning blob.
//
// See flash_storage.h for layout. NVMCTRL operations follow the datasheet
// (DS40001882) §22.6.5–22.6.7. Page buffer is filled by writing words to
// flash address space; row erase and page write are command-driven via
// NVMCTRL->CTRLA.
//
// MANW (manual write) is set so we explicitly trigger the page write — gives
// us deterministic verify timing instead of relying on auto-write happening
// at the last-word boundary.
// ============================================================================

#include "flash_storage.h"
#include "samd21.h"

#include <string.h>

#define MAGIC          0xC0FFEE00U
#define ROW_SIZE       256u   // erase granularity
#define PAGE_SIZE      64u    // write granularity
#define FLASH_SIZE     0x40000U   // 256 KB on SAMD21G18A
#define SLOT_A_ADDR    (FLASH_SIZE - 2u * ROW_SIZE)   // 0x3FE00
#define SLOT_B_ADDR    (FLASH_SIZE - 1u * ROW_SIZE)   // 0x3FF00

typedef struct {
    uint32_t magic;
    uint32_t sequence;
    uint32_t instance_id;
    uint8_t  commissioning_state;
    uint8_t  reserved[3];
} slot_t;

static const slot_t* slot_at(uint32_t addr) {
    return (const slot_t*)(uintptr_t)addr;
}

static bool slot_valid(const slot_t* s) {
    return s->magic == MAGIC;
}

static void nvm_wait_ready(void) {
    while (NVMCTRL->INTFLAG.bit.READY == 0) { /* spin */ }
}

static void nvm_erase_row(uint32_t addr) {
    nvm_wait_ready();
    // ADDR is the half-word address (byte addr / 2) per datasheet 22.8.4.
    NVMCTRL->ADDR.reg = addr / 2u;
    NVMCTRL->CTRLA.reg = NVMCTRL_CTRLA_CMD_ER | NVMCTRL_CTRLA_CMDEX_KEY;
    nvm_wait_ready();
}

static void nvm_write_page(uint32_t addr, const void* data, size_t bytes) {
    // Clear the page buffer.
    nvm_wait_ready();
    NVMCTRL->CTRLA.reg = NVMCTRL_CTRLA_CMD_PBC | NVMCTRL_CTRLA_CMDEX_KEY;
    nvm_wait_ready();

    // Disable auto-write so we control when the write fires.
    NVMCTRL->CTRLB.bit.MANW = 1;

    // Fill the page buffer via word writes into flash address space. The
    // NVMCTRL captures these into the buffer until we issue CMD_WP.
    volatile uint32_t* dst = (volatile uint32_t*)(uintptr_t)addr;
    const uint8_t* src = (const uint8_t*)data;
    uint32_t words = (bytes + 3u) / 4u;
    if (words * 4u > PAGE_SIZE) { words = PAGE_SIZE / 4u; }
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w;
        memcpy(&w, &src[i * 4u], 4u);   // tolerates unaligned tail
        dst[i] = w;
    }

    // Trigger the page write.
    NVMCTRL->ADDR.reg = addr / 2u;
    NVMCTRL->CTRLA.reg = NVMCTRL_CTRLA_CMD_WP | NVMCTRL_CTRLA_CMDEX_KEY;
    nvm_wait_ready();
}

bool flash_storage_read(commission_blob_t* out) {
    const slot_t* a = slot_at(SLOT_A_ADDR);
    const slot_t* b = slot_at(SLOT_B_ADDR);
    bool va = slot_valid(a);
    bool vb = slot_valid(b);

    const slot_t* winner = NULL;
    if (va && vb) {
        winner = (a->sequence >= b->sequence) ? a : b;
    } else if (va) {
        winner = a;
    } else if (vb) {
        winner = b;
    } else {
        return false;   // factory-fresh
    }

    if (out) {
        out->instance_id         = winner->instance_id;
        out->commissioning_state = winner->commissioning_state;
    }
    return true;
}

bool flash_storage_write(uint32_t instance_id, uint8_t commissioning_state) {
    const slot_t* a = slot_at(SLOT_A_ADDR);
    const slot_t* b = slot_at(SLOT_B_ADDR);
    bool va = slot_valid(a);
    bool vb = slot_valid(b);

    uint32_t target_addr;
    uint32_t next_seq;

    if (va && vb) {
        // Both valid — write to the older slot.
        if (a->sequence >= b->sequence) {
            target_addr = SLOT_B_ADDR;
            next_seq    = a->sequence + 1u;
        } else {
            target_addr = SLOT_A_ADDR;
            next_seq    = b->sequence + 1u;
        }
    } else if (va) {
        target_addr = SLOT_B_ADDR;
        next_seq    = a->sequence + 1u;
    } else if (vb) {
        target_addr = SLOT_A_ADDR;
        next_seq    = b->sequence + 1u;
    } else {
        // Both empty.
        target_addr = SLOT_A_ADDR;
        next_seq    = 1u;
    }

    slot_t blob = {
        .magic               = MAGIC,
        .sequence            = next_seq,
        .instance_id         = instance_id,
        .commissioning_state = commissioning_state,
        .reserved            = { 0xFF, 0xFF, 0xFF },   // match erased flash for the rest
    };

    nvm_erase_row(target_addr);
    nvm_write_page(target_addr, &blob, sizeof(blob));

    // Verify by reading back through the flash address.
    const slot_t* check = slot_at(target_addr);
    return check->magic == MAGIC
        && check->sequence == next_seq
        && check->instance_id == instance_id
        && check->commissioning_state == commissioning_state;
}
