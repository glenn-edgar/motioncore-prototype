// ============================================================================
// gfx.c — monochrome graphics primitives. See gfx.h.
// ============================================================================

#include "gfx.h"
#include "font5x7.h"

void gfx_init(gfx_t *g, uint8_t *buf, int16_t width, int16_t height) {
    g->buf    = buf;
    g->width  = width;
    g->height = height;
}

void gfx_clear(gfx_t *g, uint8_t color) {
    int32_t n = (int32_t)g->width * (g->height / 8);
    uint8_t v = (color == GFX_WHITE) ? 0xFFu : 0x00u;
    if (color == GFX_INVERT) {
        for (int32_t i = 0; i < n; i++) g->buf[i] ^= 0xFFu;
    } else {
        for (int32_t i = 0; i < n; i++) g->buf[i] = v;
    }
}

void gfx_draw_pixel(gfx_t *g, int16_t x, int16_t y, uint8_t color) {
    if (x < 0 || x >= g->width || y < 0 || y >= g->height) return;
    uint8_t *p = &g->buf[(int32_t)x + (int32_t)(y >> 3) * g->width];
    uint8_t  m = (uint8_t)(1u << (y & 7));
    switch (color) {
        case GFX_WHITE:  *p |=  m; break;
        case GFX_BLACK:  *p &= (uint8_t)~m; break;
        default:         *p ^=  m; break;  // GFX_INVERT
    }
}

void gfx_draw_hline(gfx_t *g, int16_t x, int16_t y, int16_t w, uint8_t color) {
    if (w < 0) { x += (int16_t)(w + 1); w = (int16_t)-w; }
    for (int16_t i = 0; i < w; i++) gfx_draw_pixel(g, (int16_t)(x + i), y, color);
}

void gfx_draw_vline(gfx_t *g, int16_t x, int16_t y, int16_t h, uint8_t color) {
    if (h < 0) { y += (int16_t)(h + 1); h = (int16_t)-h; }
    for (int16_t i = 0; i < h; i++) gfx_draw_pixel(g, x, (int16_t)(y + i), color);
}

// Bresenham.
void gfx_draw_line(gfx_t *g, int16_t x0, int16_t y0,
                   int16_t x1, int16_t y1, uint8_t color) {
    int16_t dx = (int16_t)(x1 - x0); if (dx < 0) dx = (int16_t)-dx;
    int16_t dy = (int16_t)(y1 - y0); if (dy < 0) dy = (int16_t)-dy;
    int16_t sx = (x0 < x1) ? 1 : -1;
    int16_t sy = (y0 < y1) ? 1 : -1;
    int16_t err = (int16_t)(dx - dy);
    for (;;) {
        gfx_draw_pixel(g, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int16_t e2 = (int16_t)(2 * err);
        if (e2 > -dy) { err = (int16_t)(err - dy); x0 = (int16_t)(x0 + sx); }
        if (e2 <  dx) { err = (int16_t)(err + dx); y0 = (int16_t)(y0 + sy); }
    }
}

void gfx_draw_rect(gfx_t *g, int16_t x, int16_t y,
                   int16_t w, int16_t h, uint8_t color) {
    gfx_draw_hline(g, x, y, w, color);
    gfx_draw_hline(g, x, (int16_t)(y + h - 1), w, color);
    gfx_draw_vline(g, x, y, h, color);
    gfx_draw_vline(g, (int16_t)(x + w - 1), y, h, color);
}

void gfx_fill_rect(gfx_t *g, int16_t x, int16_t y,
                   int16_t w, int16_t h, uint8_t color) {
    for (int16_t i = 0; i < w; i++)
        gfx_draw_vline(g, (int16_t)(x + i), y, h, color);
}

// Midpoint circle.
void gfx_draw_circle(gfx_t *g, int16_t cx, int16_t cy, int16_t r, uint8_t color) {
    int16_t x = 0, y = r;
    int16_t d = (int16_t)(1 - r);
    while (x <= y) {
        gfx_draw_pixel(g, (int16_t)(cx + x), (int16_t)(cy + y), color);
        gfx_draw_pixel(g, (int16_t)(cx - x), (int16_t)(cy + y), color);
        gfx_draw_pixel(g, (int16_t)(cx + x), (int16_t)(cy - y), color);
        gfx_draw_pixel(g, (int16_t)(cx - x), (int16_t)(cy - y), color);
        gfx_draw_pixel(g, (int16_t)(cx + y), (int16_t)(cy + x), color);
        gfx_draw_pixel(g, (int16_t)(cx - y), (int16_t)(cy + x), color);
        gfx_draw_pixel(g, (int16_t)(cx + y), (int16_t)(cy - x), color);
        gfx_draw_pixel(g, (int16_t)(cx - y), (int16_t)(cy - x), color);
        x++;
        if (d < 0) {
            d = (int16_t)(d + 2 * x + 1);
        } else {
            y--;
            d = (int16_t)(d + 2 * (x - y) + 1);
        }
    }
}

void gfx_fill_circle(gfx_t *g, int16_t cx, int16_t cy, int16_t r, uint8_t color) {
    int16_t x = 0, y = r;
    int16_t d = (int16_t)(1 - r);
    while (x <= y) {
        // Horizontal spans between the symmetric octant points.
        gfx_draw_hline(g, (int16_t)(cx - x), (int16_t)(cy + y), (int16_t)(2 * x + 1), color);
        gfx_draw_hline(g, (int16_t)(cx - x), (int16_t)(cy - y), (int16_t)(2 * x + 1), color);
        gfx_draw_hline(g, (int16_t)(cx - y), (int16_t)(cy + x), (int16_t)(2 * y + 1), color);
        gfx_draw_hline(g, (int16_t)(cx - y), (int16_t)(cy - x), (int16_t)(2 * y + 1), color);
        x++;
        if (d < 0) {
            d = (int16_t)(d + 2 * x + 1);
        } else {
            y--;
            d = (int16_t)(d + 2 * (x - y) + 1);
        }
    }
}

void gfx_draw_char(gfx_t *g, int16_t x, int16_t y, char c,
                   uint8_t color, uint8_t bg, uint8_t size) {
    uint8_t uc = (uint8_t)c;
    if (uc < FONT5X7_FIRST || uc > FONT5X7_LAST) uc = (uint8_t)'?';
    const uint8_t *glyph = &FONT5X7[(uc - FONT5X7_FIRST) * 5];
    if (size == 0u) size = 1u;

    // 6 columns (5 glyph + 1 spacing) x 8 rows (7 glyph + 1 spacing).
    for (int16_t col = 0; col < 6; col++) {
        uint8_t bits = (col < 5) ? glyph[col] : 0x00u;
        for (int16_t row = 0; row < 8; row++) {
            uint8_t on  = (uint8_t)((bits >> row) & 1u);
            uint8_t col_ = on ? color : bg;
            if (on || bg != color) {  // skip background draw when bg==color
                if (size == 1u) {
                    gfx_draw_pixel(g, (int16_t)(x + col), (int16_t)(y + row), col_);
                } else {
                    gfx_fill_rect(g, (int16_t)(x + col * size), (int16_t)(y + row * size),
                                  size, size, col_);
                }
            }
        }
    }
}

int16_t gfx_draw_text(gfx_t *g, int16_t x, int16_t y, const char *s,
                      uint8_t color, uint8_t bg, uint8_t size) {
    if (size == 0u) size = 1u;
    int16_t advance = (int16_t)(6 * size);
    for (; *s; s++) {
        gfx_draw_char(g, x, y, *s, color, bg, size);
        x = (int16_t)(x + advance);
    }
    return x;
}
