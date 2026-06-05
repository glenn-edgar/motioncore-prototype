// ============================================================================
// ssd1306.h — Solomon SSD1306 monochrome OLED over I2C, with the bundled gfx
// pixel library.
//
// Supports the two common panels: 128x64 and 128x32. The framebuffer lives
// inside the driver (one static 128x64 = 1024 B buffer — sized for the larger
// panel; a 128x32 panel uses the first 512 B). Draw into it with the gfx_*
// API via ssd1306_gfx(), then push it to the panel with ssd1306_display().
//
// Wire protocol: every I2C transaction starts with a control byte —
//   0x00 -> the following bytes are COMMANDS
//   0x40 -> the following bytes are DISPLAY RAM (GDDRAM) data
// (Co=0, so the whole burst is one type.) See i2c_bus_write_prefixed().
//
// 7-bit address: 0x3C (default) or 0x3D (SA0 high). Assumes the internal
// charge pump (no external VCC), which covers essentially all breakout boards.
//
// Single panel per build — the framebuffer is static. If you ever need two,
// promote the buffer into ssd1306_t and pass it in ssd1306_init().
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "gfx.h"

#define SSD1306_ADDR_PRIMARY   0x3Cu
#define SSD1306_ADDR_SECONDARY 0x3Du

typedef struct {
    uint8_t addr;     // 7-bit
    uint8_t width;    // 128
    uint8_t height;   // 64 or 32
    uint8_t pages;    // height / 8
    gfx_t   gfx;      // bound to the internal framebuffer
} ssd1306_t;

// Run the power-on init sequence for a width×height panel (use 128 and 64 or
// 32), clear the framebuffer, and turn the display on. Returns false if the
// panel does not ACK on the bus or height is unsupported.
bool ssd1306_init(ssd1306_t *d, uint8_t addr7, uint8_t width, uint8_t height);

// Drawing surface — pass to the gfx_* functions.
gfx_t *ssd1306_gfx(ssd1306_t *d);

// Push the whole framebuffer to GDDRAM. Call after drawing.
bool ssd1306_display(ssd1306_t *d);

// Convenience: clear the framebuffer (does not push — call ssd1306_display).
void ssd1306_clear(ssd1306_t *d);

// Panel controls (take effect immediately).
bool ssd1306_set_contrast(ssd1306_t *d, uint8_t contrast);  // 0..255
bool ssd1306_invert(ssd1306_t *d, bool invert);             // inverse video
bool ssd1306_power(ssd1306_t *d, bool on);                  // sleep / wake
