// ============================================================================
// gfx.h — minimal monochrome graphics ("pixel") library.
//
// Display-agnostic: it draws into a 1-bit-per-pixel framebuffer laid out in
// the SSD1306/page format used by most small OLED/LCD controllers —
//
//     byte index = x + (y / 8) * width
//     bit  index = y & 7        (bit 0 = topmost of the 8-px column)
//
// i.e. each byte is a vertical run of 8 pixels. The SSD1306 GDDRAM is exactly
// this, so ssd1306_display() is a straight memcpy of the framebuffer.
//
// API and the bundled 5x7 font mirror the long-standing Adafruit-GFX style so
// it's familiar, but this is plain C with no dynamic allocation. Bind a gfx_t
// to a buffer once, then draw; the owner (e.g. ssd1306) flushes it to the panel.
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Pixel "colors" for a 1bpp panel.
enum {
    GFX_BLACK  = 0,   // pixel off
    GFX_WHITE  = 1,   // pixel on
    GFX_INVERT = 2,   // toggle the pixel
};

typedef struct {
    uint8_t *buf;     // framebuffer, width * (height/8) bytes
    int16_t  width;   // pixels
    int16_t  height;  // pixels (multiple of 8)
} gfx_t;

// Bind a framebuffer. height must be a multiple of 8. Does not clear.
void gfx_init(gfx_t *g, uint8_t *buf, int16_t width, int16_t height);

// Fill the whole buffer (GFX_BLACK or GFX_WHITE; GFX_INVERT flips every pixel).
void gfx_clear(gfx_t *g, uint8_t color);

// Single pixel. Off-screen coordinates are silently clipped.
void gfx_draw_pixel(gfx_t *g, int16_t x, int16_t y, uint8_t color);

// Lines.
void gfx_draw_hline(gfx_t *g, int16_t x, int16_t y, int16_t w, uint8_t color);
void gfx_draw_vline(gfx_t *g, int16_t x, int16_t y, int16_t h, uint8_t color);
void gfx_draw_line(gfx_t *g, int16_t x0, int16_t y0,
                   int16_t x1, int16_t y1, uint8_t color);

// Rectangles.
void gfx_draw_rect(gfx_t *g, int16_t x, int16_t y,
                   int16_t w, int16_t h, uint8_t color);
void gfx_fill_rect(gfx_t *g, int16_t x, int16_t y,
                   int16_t w, int16_t h, uint8_t color);

// Circles (midpoint).
void gfx_draw_circle(gfx_t *g, int16_t cx, int16_t cy, int16_t r, uint8_t color);
void gfx_fill_circle(gfx_t *g, int16_t cx, int16_t cy, int16_t r, uint8_t color);

// Text. The font is 5x7 in a 6x8 cell (1 px right + bottom spacing). `size`
// scales each font pixel into a size×size block (1 = native 6x8). `bg` is the
// background color; pass it equal to `color` to draw glyph pixels only
// ("transparent" background is not supported — clear first if you need that).
void gfx_draw_char(gfx_t *g, int16_t x, int16_t y, char c,
                   uint8_t color, uint8_t bg, uint8_t size);

// Draw a NUL-terminated string left-to-right starting at (x,y). No wrapping;
// returns the x just past the last glyph so callers can chain.
int16_t gfx_draw_text(gfx_t *g, int16_t x, int16_t y, const char *s,
                      uint8_t color, uint8_t bg, uint8_t size);
