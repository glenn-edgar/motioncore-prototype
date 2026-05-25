// ============================================================================
// samd21_pin_table.c — Seeeduino Xiao SAMD21G18A board-label table.
//
// Physical pin allocations per Seeed XIAO SAMD21 wiki + SAMD21G18A datasheet:
//
//   Label  Port/Pin   ADC channel   Alt function
//   -----  ---------  -----------   -------------
//   D0     PA02       AIN0          DAC output (RESERVED — always-on)
//   D1     PA04       AIN4          —
//   D2     PA10       AIN18         —
//   D3     PA11       AIN19         —
//   D4     PA08       AIN16         SDA / SERCOM2 (RESERVED — always-on I2C)
//   D5     PA09       AIN17         SCL / SERCOM2 (RESERVED — always-on I2C)
//   D6     PB08       AIN2          TX  / SERCOM4 (RESERVED — UART)
//   D7     PB09       AIN3          RX  / SERCOM4 (RESERVED — UART)
//   D8     PA07       AIN7          (SCK on Arduino headers, unused here)
//   D9     PA05       AIN5          (MISO on Arduino headers, unused here)
//   D10    PA06       AIN6          (MOSI on Arduino headers, unused here)
//
// A0..A10 alias D0..D10 with the same physical pin; the alias exists so the
// DSL can mark intent (digital vs analog mode). The HAL claim layer treats
// (D2, A2) as the same resource via physical_pin_id.
// ============================================================================

#include "samd21_pin_table.h"
#include <string.h>

#define CAP_FREE_GPIO    (BOARD_PIN_CAP_GPIO | BOARD_PIN_CAP_ADC)
#define CAP_RESV_DAC     (BOARD_PIN_CAP_GPIO | BOARD_PIN_CAP_ADC | BOARD_PIN_CAP_DAC | BOARD_PIN_RESERVED)
#define CAP_RESV_PERI    (BOARD_PIN_CAP_GPIO | BOARD_PIN_CAP_ADC | BOARD_PIN_RESERVED)

const board_pin_t g_board_pins[] = {
    // D-labels (digital intent)
    { "D0",  0,  2, 0u,                CAP_RESV_DAC  },
    { "D1",  0,  4, 4u,                CAP_FREE_GPIO },
    { "D2",  0, 10, 18u,               CAP_FREE_GPIO },
    { "D3",  0, 11, 19u,               CAP_FREE_GPIO },
    { "D4",  0,  8, 16u,               CAP_RESV_PERI },
    { "D5",  0,  9, 17u,               CAP_RESV_PERI },
    { "D6",  1,  8, 2u,                CAP_RESV_PERI },
    { "D7",  1,  9, 3u,                CAP_RESV_PERI },
    { "D8",  0,  7, 7u,                CAP_FREE_GPIO },
    { "D9",  0,  5, 5u,                CAP_FREE_GPIO },
    { "D10", 0,  6, 6u,                CAP_FREE_GPIO },

    // A-labels (analog intent) — same physical pin as the D counterpart
    { "A0",  0,  2, 0u,                CAP_RESV_DAC  },
    { "A1",  0,  4, 4u,                CAP_FREE_GPIO },
    { "A2",  0, 10, 18u,               CAP_FREE_GPIO },
    { "A3",  0, 11, 19u,               CAP_FREE_GPIO },
    { "A4",  0,  8, 16u,               CAP_RESV_PERI },
    { "A5",  0,  9, 17u,               CAP_RESV_PERI },
    { "A6",  1,  8, 2u,                CAP_RESV_PERI },
    { "A7",  1,  9, 3u,                CAP_RESV_PERI },
    { "A8",  0,  7, 7u,                CAP_FREE_GPIO },
    { "A9",  0,  5, 5u,                CAP_FREE_GPIO },
    { "A10", 0,  6, 6u,                CAP_FREE_GPIO },
};

const uint8_t g_board_pin_count = (uint8_t)(sizeof(g_board_pins) / sizeof(g_board_pins[0]));

const board_pin_t* board_pin_lookup(const char* label, uint8_t label_len) {
    if (label == 0 || label_len == 0u || label_len >= sizeof(g_board_pins[0].label)) {
        return 0;
    }
    for (uint8_t i = 0; i < g_board_pin_count; i++) {
        const board_pin_t* p = &g_board_pins[i];
        // Match exact length (strlen of stored label == label_len).
        if (strlen(p->label) != label_len) continue;
        if (memcmp(p->label, label, label_len) == 0) return p;
    }
    return 0;
}
