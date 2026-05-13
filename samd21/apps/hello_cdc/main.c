// hello_cdc — Seeeduino Xiao SAMD21
// Prints "hello SAMD21 tick=N\n" once a second over USB-CDC.

#include <stdint.h>
#include <stdio.h>

#include "bsp/board_api.h"
#include "tusb.h"

// SysTick ticks at 1 kHz (configured in family.c board_init); board_millis()
// is the documented BSP accessor.

int main(void) {
  board_init();

  // TinyUSB 0.18.x: explicit rhport+role init.
  tusb_rhport_init_t const rhport_init = {
      .role  = TUSB_ROLE_DEVICE,
      .speed = TUSB_SPEED_AUTO,
  };
  tusb_init(BOARD_TUD_RHPORT, &rhport_init);

  uint32_t next_print_ms = 1000;
  uint32_t tick_counter  = 0;

  for (;;) {
    tud_task();  // tinyusb device task

    // Drain anything already buffered into the CDC pipe.
    if (tud_cdc_connected()) {
      tud_cdc_write_flush();
    }

    uint32_t now = board_millis();
    if ((int32_t)(now - next_print_ms) >= 0) {
      next_print_ms += 1000;

      // printf goes to _write -> CDC.  If host hasn't opened the port yet,
      // tinyusb just drops the bytes (tud_cdc_write returns 0).
      printf("hello SAMD21 tick=%lu\r\n", (unsigned long) tick_counter++);
      fflush(stdout);

      if (tud_cdc_connected()) {
        tud_cdc_write_flush();
      }

      // Blink the LED so we can confirm the firmware is alive without USB.
      static bool led_on = false;
      led_on = !led_on;
      board_led_write(led_on);
    }
  }
}
