# 74HC4067 — 16-channel analog mux/demux (SAMD21)

GPIO-driven driver for the 74HC4067 analog multiplexer. Unlike the parts under
`samd21/i2c/`, this chip has **no register interface and no I2C** — it's a
combinational analog switch steered by digital pins:

```
        S0 S1 S2 S3   four address lines  (S0 = LSB of the channel number)
        E̅             active-low enable   (high = all channels off / Hi-Z)
        SIG (Z)       common pin -> wire to a SAMD21 ADC channel (or DAC)
        Y0..Y15       the 16 analog signals being multiplexed
```

The driver drives S0..S3 and E̅ through the SAMD21 **PORT** registers
(`DIRSET` to make outputs, `OUTSET`/`OUTCLR` to set levels). Reading the
selected signal is left to the caller's ADC — the `hc4067_scan()` helper takes
an ADC-read callback so it stays decoupled from any particular ADC setup.

Because the switch is bidirectional, the same API works for 1→16 **demux**
(drive SIG from a DAC, fan out to Y0..Y15).

## Pin coordinates

Pins use the repo's compact `phys_id = (port << 5) | pin` (same as
`samd21_pin_table.h`). Build them with `HC4067_PA(n)` / `HC4067_PB(n)`, or pass
`HC4067_NO_PIN` for E̅ if it's hardwired to GND (always enabled).

## Usage

```c
#include "hc4067.h"

hc4067_t mux;
// S0=PA4, S1=PA5, S2=PA6, S3=PA7, E̅=PA8 (active-low)
hc4067_init(&mux, HC4067_PA(4), HC4067_PA(5), HC4067_PA(6), HC4067_PA(7),
            HC4067_PA(8), /*en_active_low=*/true);

// Single channel: select, settle, read SIG via your own ADC.
hc4067_select_enable(&mux, 9);
hc4067_settle(96);                 // ~2 µs at 48 MHz
uint16_t code = my_adc_read();     // sample the SIG pin

// Or sweep all 16 into a buffer with one call:
uint16_t samples[16];
hc4067_scan(&mux, 0, 16, samples, my_adc_read_cb, /*ctx=*/NULL, /*settle=*/96);
```

`my_adc_read_cb` has signature `uint16_t (*)(void *ctx)` and should sample the
SIG pin (e.g. wrap `samd21_adc_read_oneshot()` on the SIG channel).

## Building / testing

`hc4067.c` needs the SAMD21 CMSIS header `samd21.h` (for the PORT registers),
so it compiles with the same `arm-none-eabi-gcc` flags / include paths as the
`register_dongle` app. The channel-decode, enable-polarity, and scan
ordering/clamping logic was unit-tested off-target (LSB-first address mapping,
active-low E̅, `[first, 16)` clamp). The header is portable
(`stdint`/`stdbool`).
