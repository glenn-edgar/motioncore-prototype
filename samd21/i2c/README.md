# SAMD21 I²C device drivers

Bare-metal C drivers for the I²C parts on the slow bus, written against SAMD21
registers (the bus layer) and each chip's own register map (the per-chip
drivers). No HAL/Arduino dependency — they sit directly on a SERCOM2 I²C master.

## Layout

```
common/i2c_bus.{h,c}   SERCOM2 I²C master transport (SAMD21 registers)
mcp23017/              Microchip MCP23017 — 16-bit GPIO expander (register-mapped)
tca9555/               TI TCA9555     — 16-bit GPIO expander (PCA9555-compatible)
pcf8575/               NXP/TI PCF8575 — 16-bit quasi-bidirectional I/O (pointer-less)
sx1509/sx1509.{h,c}    Semtech SX1509 — 16 I/O + internal osc + 256-step LED driver
ads1115/ads1115.{h,c}  TI ADS1115     — 16-bit 4-ch ADC (not an expander, shares bus)
ssd1306/ssd1306.{h,c}  Solomon SSD1306 — 128x64/128x32 monochrome OLED
ssd1306/gfx.{h,c}      pixel library — clear/pixel/line/rect/circle/text (1bpp)
ssd1306/font5x7.h      classic 5x7 ASCII font (0x20..0x7E)
```

> Each subdirectory is named for its part; the source files use the same part
> number (e.g. `sx1509/sx1509.c`, `ads1115/ads1115.c`).

## Bus

`common/i2c_bus.c` brings up **SERCOM2** as a 100 kHz I²C master on:

| Signal | Pin  | Port | SERCOM2 pad | PMUX |
|--------|------|------|-------------|------|
| SDA    | D4   | PA08 | PAD[0]      | D    |
| SCL    | D5   | PA09 | PAD[1]      | D    |

This is the same SERCOM2/pin assignment the `register_dongle` app reserves for
"always-on I²C". The code was lifted from that app's I²C block and given a
**bounded busy-wait** (`I2C_SPIN_TIMEOUT`) so a missing slave returns `false`
instead of relying on the layer-2 WDT to recover a wedged bus.

`i2c_bus_init()` is idempotent. If you link these drivers into the
`register_dongle` app — which already initialises SERCOM2 in
`samd21_peripherals_init()` — the bus is shared and you can skip the call; the
per-chip `*_init()` functions call `i2c_bus_init()` anyway, and the second call
is a no-op.

## 7-bit addresses

| Chip      | Range / pins                                   |
|-----------|------------------------------------------------|
| MCP23017  | `0x20`–`0x27` (A2 A1 A0)                        |
| TCA9555   | `0x20`–`0x27` (A2 A1 A0)                        |
| PCF8575   | `0x20`–`0x27` (A2 A1 A0)                        |
| SX1509    | `0x3E`, `0x3F`, `0x70`, `0x71` (ADDR1 ADDR0)    |
| ADS1115   | `0x48`–`0x4B` (ADDR → GND/VDD/SDA/SCL)          |
| SSD1306   | `0x3C` (default), `0x3D` (SA0 high)            |

MCP23017 / TCA9555 / PCF8575 overlap `0x20`–`0x27`; don't put two of them on the
same strap on one bus.

## Direction convention

All three expanders follow their own silicon: a **1** in the direction/config
register means **input**. PCF8575 has no config register — writing a 1 to a pin
releases it (weak pull-up → usable as input); writing 0 drives it low.

## Usage sketch

```c
#include "mcp23017/mcp23017.h"
#include "ads1115/ads1115.h"

mcp23017_t io;
mcp23017_init(&io, MCP23017_ADDR(0));      // 0x20
mcp23017_set_dir(&io, 0xFF00);             // low 8 = outputs, high 8 = inputs
mcp23017_set_pullups(&io, 0xFF00);         // pull-ups on the input half
mcp23017_write(&io, 0x00A5);               // drive the output half
uint16_t pins; mcp23017_read(&io, &pins);

ads1115_t adc;
ads1115_init(&adc, ADS1115_ADDR_GND);      // 0x48
ads1115_set_pga(&adc, ADS1115_PGA_4_096V);
float v; ads1115_read_volts(&adc, ADS1115_MUX_SINGLE_0, &v);
```

### SSD1306 + pixel library

The OLED owns a static 1 KB framebuffer; you draw into it with the `gfx_*`
primitives, then push it to the panel:

```c
#include "ssd1306/ssd1306.h"

ssd1306_t oled;
ssd1306_init(&oled, SSD1306_ADDR_PRIMARY, 128, 64);   // 0x3C, 128x64
gfx_t *g = ssd1306_gfx(&oled);

gfx_clear(g, GFX_BLACK);
gfx_draw_text(g, 0, 0,  "MotionCore", GFX_WHITE, GFX_BLACK, 1);
gfx_draw_text(g, 0, 16, "RS-485 OK",  GFX_WHITE, GFX_BLACK, 2);  // 2x scale
gfx_draw_rect(g, 0, 40, 128, 20, GFX_WHITE);
gfx_fill_circle(g, 64, 50, 6, GFX_WHITE);
ssd1306_display(&oled);                                // flush to the panel
```

The `gfx` layer is display-agnostic (any 1bpp page-mapped buffer) and ships a
5x7 ASCII font; `gfx_draw_char`/`gfx_draw_text` take an integer `size` to scale
glyphs. The framebuffer is static (one panel per build) — see the note in
`ssd1306.h` if you need two.

## Building

Each chip driver depends only on `common/i2c_bus.h` (portable: `stdint`/`stdbool`).
`common/i2c_bus.c` needs the SAMD21 CMSIS header `samd21.h` and is compiled with
the same `arm-none-eabi-gcc` flags / include paths as the `register_dongle` app
(see `samd21/apps/register_dongle/Makefile`). The chip `.c` files all pass
`gcc -std=c11 -Wall -Wextra -Wconversion -fsyntax-only` on the host.

These are pure library modules — no shell-command wiring. To expose them over
the dongle protocol, call them from a `cmd_*` handler in `samd21_commands.c`
the same way the generic `CMD_I2C_*` handlers there call the low-level bus.
