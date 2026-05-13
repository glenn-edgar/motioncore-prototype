// cdc_rx_hexdump — Seeeduino Xiao SAMD21
// Sanity-check firmware for the m2s wire path. Drains incoming CDC bytes
// each main-loop iteration and prints them back as hex over the same CDC
// pipe. Also emits a 1 Hz heartbeat so we can tell the firmware is alive
// even when no bytes are arriving from the host.
//
// Expected use:
//   1. Flash this UF2 onto the Xiao.
//   2. Open `dongle_console --hex` (or --ascii) in one shell.
//   3. From another shell: `dongle_console --send-ping` (or --send-ack).
//   4. The TX bytes (Cx 00 04 01 .. .. .. Cx) should echo back as
//      `[RX 8] C0 00 04 01 ...` in the listening console.

#include <stdint.h>
#include <stdio.h>

#include "bsp/board_api.h"
#include "tusb.h"

#define RX_BUF_SIZE 64

int main(void) {
  board_init();

  tusb_rhport_init_t const rhport_init = {
      .role  = TUSB_ROLE_DEVICE,
      .speed = TUSB_SPEED_AUTO,
  };
  tusb_init(BOARD_TUD_RHPORT, &rhport_init);

  uint32_t next_hb_ms   = 1000;
  uint32_t hb_counter   = 0;
  uint32_t rx_total     = 0;
  uint8_t  rx_buf[RX_BUF_SIZE];

  for (;;) {
    tud_task();

    // Drain anything queued on the host -> dongle pipe.
    if (tud_cdc_connected() && tud_cdc_available()) {
      uint32_t n = tud_cdc_read(rx_buf, RX_BUF_SIZE);
      if (n > 0) {
        rx_total += n;
        printf("[RX %lu]", (unsigned long) n);
        for (uint32_t i = 0; i < n; i++) {
          printf(" %02X", (unsigned) rx_buf[i]);
        }
        printf("\r\n");
        fflush(stdout);
        tud_cdc_write_flush();
      }
    }

    uint32_t now = board_millis();
    if ((int32_t)(now - next_hb_ms) >= 0) {
      next_hb_ms += 1000;
      printf("[hb] tick=%lu rx_total=%lu\r\n",
             (unsigned long) hb_counter++,
             (unsigned long) rx_total);
      fflush(stdout);
      if (tud_cdc_connected()) tud_cdc_write_flush();

      static bool led_on = false;
      led_on = !led_on;
      board_led_write(led_on);
    }
  }
}
