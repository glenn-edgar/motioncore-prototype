// ============================================================================
// hello_cdc — Seeed XIAO RA4M1 (Renesas R7FA4M1AB, Cortex-M4)
//
// First-milestone bring-up: enumerate USB-CDC and emit a 1 Hz counter line.
// Proves the RA4M1 toolchain + FSP + TinyUSB + DFU-flash pipeline end to end,
// the way 00_hello_cdc did for the SAMD21.
//
// board_init() (TinyUSB's hw/bsp/ra/family.c) handles clock + USB pin setup.
// ============================================================================

#include <stdio.h>
#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"

int main(void) {
    board_init();

    tusb_rhport_init_t dev_init = {
        .role  = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO,
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    uint32_t next_ms = 1000;
    uint32_t tick    = 0;

    for (;;) {
        tud_task();

        if (board_millis() >= next_ms) {
            next_ms += 1000;
            if (tud_cdc_connected()) {
                char line[48];
                int n = snprintf(line, sizeof line,
                                 "hello RA4M1 tick=%lu\r\n",
                                 (unsigned long)tick);
                if (n > 0) {
                    tud_cdc_write(line, (uint32_t)n);
                    tud_cdc_write_flush();
                }
            }
            tick++;
        }
    }
}
