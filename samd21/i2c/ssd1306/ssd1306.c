// ============================================================================
// ssd1306.c — Solomon SSD1306 OLED driver. See ssd1306.h.
// ============================================================================

#include "ssd1306.h"
#include "../common/i2c_bus.h"

// Control-byte prefixes.
#define SSD1306_CTRL_CMD    0x00u
#define SSD1306_CTRL_DATA   0x40u

// Command opcodes used here.
#define SSD1306_SET_CONTRAST        0x81
#define SSD1306_DISPLAY_RAM         0xA4
#define SSD1306_DISPLAY_NORMAL      0xA6
#define SSD1306_DISPLAY_INVERT      0xA7
#define SSD1306_DISPLAY_OFF         0xAE
#define SSD1306_DISPLAY_ON          0xAF
#define SSD1306_SET_DISP_CLOCKDIV   0xD5
#define SSD1306_SET_MULTIPLEX       0xA8
#define SSD1306_SET_DISP_OFFSET     0xD3
#define SSD1306_SET_START_LINE      0x40
#define SSD1306_CHARGE_PUMP         0x8D
#define SSD1306_MEMORY_MODE         0x20
#define SSD1306_SEG_REMAP           0xA1   // column 127 -> SEG0
#define SSD1306_COM_SCAN_DEC        0xC8
#define SSD1306_SET_COM_PINS        0xDA
#define SSD1306_SET_PRECHARGE       0xD9
#define SSD1306_SET_VCOM_DETECT     0xDB
#define SSD1306_SET_COLUMN_ADDR     0x21
#define SSD1306_SET_PAGE_ADDR       0x22

// Static framebuffer, sized for the largest supported panel (128x64).
#define SSD1306_FB_MAX  (128u * 64u / 8u)   // 1024
static uint8_t s_fb[SSD1306_FB_MAX];

static bool cmd1(ssd1306_t *d, uint8_t c) {
    return i2c_bus_write_prefixed(d->addr, SSD1306_CTRL_CMD, &c, 1);
}
static bool cmd2(ssd1306_t *d, uint8_t c, uint8_t a) {
    uint8_t b[2] = { c, a };
    return i2c_bus_write_prefixed(d->addr, SSD1306_CTRL_CMD, b, 2);
}

bool ssd1306_init(ssd1306_t *d, uint8_t addr7, uint8_t width, uint8_t height) {
    i2c_bus_init();
    if (width != 128u || (height != 64u && height != 32u)) return false;

    d->addr   = addr7;
    d->width  = width;
    d->height = height;
    d->pages  = (uint8_t)(height / 8u);
    gfx_init(&d->gfx, s_fb, (int16_t)width, (int16_t)height);

    // Presence probe.
    if (!i2c_bus_write(d->addr, 0, 0)) return false;

    // Panel-dependent values: 64-line vs 32-line.
    uint8_t mux      = (uint8_t)(height - 1u);            // 0x3F or 0x1F
    uint8_t com_pins = (height == 64u) ? 0x12u : 0x02u;   // alt vs sequential

    // Power-on init (internal charge pump, horizontal addressing).
    bool ok = true;
    ok &= cmd1(d, SSD1306_DISPLAY_OFF);
    ok &= cmd2(d, SSD1306_SET_DISP_CLOCKDIV, 0x80);
    ok &= cmd2(d, SSD1306_SET_MULTIPLEX, mux);
    ok &= cmd2(d, SSD1306_SET_DISP_OFFSET, 0x00);
    ok &= cmd1(d, (uint8_t)(SSD1306_SET_START_LINE | 0x00));
    ok &= cmd2(d, SSD1306_CHARGE_PUMP, 0x14);             // enable internal pump
    ok &= cmd2(d, SSD1306_MEMORY_MODE, 0x00);            // horizontal addressing
    ok &= cmd1(d, SSD1306_SEG_REMAP);
    ok &= cmd1(d, SSD1306_COM_SCAN_DEC);
    ok &= cmd2(d, SSD1306_SET_COM_PINS, com_pins);
    ok &= cmd2(d, SSD1306_SET_CONTRAST, 0xCF);
    ok &= cmd2(d, SSD1306_SET_PRECHARGE, 0xF1);
    ok &= cmd2(d, SSD1306_SET_VCOM_DETECT, 0x40);
    ok &= cmd1(d, SSD1306_DISPLAY_RAM);                  // follow RAM, not all-on
    ok &= cmd1(d, SSD1306_DISPLAY_NORMAL);
    ok &= cmd1(d, SSD1306_DISPLAY_ON);
    if (!ok) return false;

    ssd1306_clear(d);
    return ssd1306_display(d);
}

gfx_t *ssd1306_gfx(ssd1306_t *d) { return &d->gfx; }

void ssd1306_clear(ssd1306_t *d) { gfx_clear(&d->gfx, GFX_BLACK); }

bool ssd1306_display(ssd1306_t *d) {
    // Address the full RAM window, then stream the framebuffer as one data burst.
    uint8_t win[6] = {
        SSD1306_SET_COLUMN_ADDR, 0x00, (uint8_t)(d->width - 1u),
        SSD1306_SET_PAGE_ADDR,   0x00, (uint8_t)(d->pages - 1u),
    };
    if (!i2c_bus_write_prefixed(d->addr, SSD1306_CTRL_CMD, win, 6)) return false;

    size_t n = (size_t)d->width * d->pages;
    return i2c_bus_write_prefixed(d->addr, SSD1306_CTRL_DATA, s_fb, n);
}

bool ssd1306_set_contrast(ssd1306_t *d, uint8_t contrast) {
    return cmd2(d, SSD1306_SET_CONTRAST, contrast);
}

bool ssd1306_invert(ssd1306_t *d, bool invert) {
    return cmd1(d, invert ? SSD1306_DISPLAY_INVERT : SSD1306_DISPLAY_NORMAL);
}

bool ssd1306_power(ssd1306_t *d, bool on) {
    return cmd1(d, on ? SSD1306_DISPLAY_ON : SSD1306_DISPLAY_OFF);
}
