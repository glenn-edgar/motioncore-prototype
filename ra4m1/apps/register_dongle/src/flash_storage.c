// ============================================================================
// flash_storage.c — RA4M1 data-flash commissioning blob (FSP r_flash_lp).
//
// See flash_storage.h for the dual-slot layout. The slot-selection and
// rotation logic is identical to the SAMD21 port — only the storage region
// (the dedicated 8 KB data flash at 0x40100000) and the erase/write
// primitives are RA4M1-specific.
//
// We drive the data flash through FSP's low-power flash HAL rather than the
// FACI registers bare (as the SAMD21 port does for NVMCTRL): the HAL handles
// the data-flash mode entry and the erase/write/blank sequencing internally,
// which is the easy thing to get subtly wrong by hand.
//
// TODO-verify (Pi) — items that cannot be confirmed from the WSL checkout
// (the FSP tree lives only on the Pi); see register_dongle/README.md:
//   * r_flash_lp.h include path + that r_flash_lp.c is in the build (Makefile).
//   * Whether the FSP build wants flash_cfg_t.irq set to FSP_INVALID_VECTOR
//     (left zero here — unused in blocking mode).
//   * The data-flash erase-block size. The 2 KB slot spacing (flash_storage.h)
//     is chosen to be robust to any block size <= 2 KB, so this only matters
//     if RA4M1 data flash uses blocks larger than 2 KB (it does not).
// ============================================================================

#include "flash_storage.h"

#include "r_flash_lp.h"        // FSP low-power flash driver (code + data flash)

#include <string.h>

#define MAGIC           0xC0FFEE00U

// RA4M1 data flash: memory-mapped, directly readable; 8 KB at 0x40100000.
// Slots are 2 KB apart so a single-block erase of one never reaches the other.
#define DATA_FLASH_BASE 0x40100000U
#define SLOT_A_ADDR     (DATA_FLASH_BASE + 0x0000U)
#define SLOT_B_ADDR     (DATA_FLASH_BASE + 0x0800U)

typedef struct {
    uint32_t magic;
    uint32_t sequence;
    uint32_t instance_id;
    uint8_t  commissioning_state;
    uint8_t  reserved[3];
} slot_t;   // 16 bytes — a multiple of the data-flash write unit.

// ----------------------------------------------------------------------------
// FSP driver instance.
// Hand-written config: the bring-up board (xiao_ra4m1, copied from uno_r4) was
// not regenerated through the FSP configurator, so there is no g_flash0 in
// ra_gen/. Blocking mode (data_flash_bgo = false) — flash_storage_write runs
// from the commissioning chain handler and a short blocking stall before the
// deferred reboot is acceptable.
// ----------------------------------------------------------------------------
static flash_lp_instance_ctrl_t g_flash_ctrl;

static const flash_cfg_t g_flash_cfg = {
    .data_flash_bgo = false,   // blocking erase/write — no callback / IRQ used
    .p_callback     = NULL,
    .p_context      = NULL,
    // p_extend / ipl / irq: zero-init — unused in blocking mode.
};

// Open the driver lazily on first write. Read is a plain memory-mapped access
// and needs no open, so the boot-time flash_storage_read() path stays cheap.
static bool ensure_open(void) {
    static bool opened = false;
    if (!opened) {
        if (R_FLASH_LP_Open(&g_flash_ctrl, &g_flash_cfg) != FSP_SUCCESS) {
            return false;
        }
        opened = true;
    }
    return true;
}

// ----------------------------------------------------------------------------
// Dual-slot logic — chip-agnostic; identical to the SAMD21 port.
// ----------------------------------------------------------------------------

static const slot_t* slot_at(uint32_t addr) {
    return (const slot_t*)(uintptr_t)addr;
}

static bool slot_valid(const slot_t* s) {
    return s->magic == MAGIC;
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
        .reserved            = { 0xFF, 0xFF, 0xFF },   // match erased flash
    };

    if (!ensure_open()) {
        return false;
    }

    // Erase the target slot's block, then write the blob. One block covers the
    // 16-byte slot with room to spare; the 2 KB slot spacing keeps the other
    // slot in a different block, so it is untouched.
    if (R_FLASH_LP_Erase(&g_flash_ctrl, target_addr, 1) != FSP_SUCCESS) {
        return false;
    }
    if (R_FLASH_LP_Write(&g_flash_ctrl, (uint32_t)(uintptr_t)&blob,
                         target_addr, sizeof(slot_t)) != FSP_SUCCESS) {
        return false;
    }

    // Verify by reading back through the memory-mapped data-flash address.
    const slot_t* check = slot_at(target_addr);
    return check->magic == MAGIC
        && check->sequence == next_seq
        && check->instance_id == instance_id
        && check->commissioning_state == commissioning_state;
}
