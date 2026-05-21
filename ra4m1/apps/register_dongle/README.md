# register_dongle — RA4M1 dongle firmware (step 3b)

The SAMD21 `register_dongle` ported to the Seeed XIAO RA4M1 (Renesas
R7FA4M1AB, Cortex-M4). Speaks the libcomm wire protocol to a Linux host over
USB-CDC, runs the four-layer sync ladder, and exposes a binary command shell.

This is the **second** chip in the four-chip dongle suite. It reuses the
engine, the chain ROM, the general shell layer, and libcomm unchanged from the
SAMD21 reference; only the chip layer (flash storage, sysinfo, UID, the DFU
touch) is RA4M1-specific.

> **Status (2026-05-21): hardware-verified on the XIAO RA4M1.** Boots, walks
> the full four-layer sync ladder (BOOT → L1_DONE → L2 → OPERATIONAL),
> commissions to the data flash with persistence across reboot *and* reflash,
> and round-trips the general app-shell (`CMD_ECHO`, `CMD_SYSINFO`). Two bugs
> found + fixed during bring-up — see "Hardware verification". The only item
> still open is the 1200-baud DFU magic (placeholder; flashing uses raflash).

---

## Architecture

Identical to the SAMD21 port — see `samd21/apps/register_dongle/README.md` for
the four-layer protocol, the dongle state machine
(`UNCOMMISSIONED → BOOT → L1_DONE → OPERATIONAL`), and the app-shell command
wire format.

Step 3b dispatches the **general** shell layer only (`CMD_ECHO`,
`CMD_SYSINFO`). `ra4m1_commands.c` is a NULL stub; the RA4M1 analytical-HIL
command set (ADC/DAC/PWM/encoder) is **step 4**.

---

## Build

```sh
cd ra4m1/apps/register_dongle
make BOARD=xiao_ra4m1
# -> _build/xiao_ra4m1/register_dongle.bin
```

Flash via the Renesas USB Boot ROM + `raflash` (hold BOOT during USB power-up):

```sh
cd ~/raflash
sudo .venv/bin/raflash erase --start_address 0x4000 --size 0x4000 --port /dev/ttyACM0
sudo .venv/bin/raflash write --start_address 0x4000 --port /dev/ttyACM0 \
     <path>/register_dongle.bin
# tap RESET -> Seeed bootloader -> app @0x4000
```

Once the 1200-baud-touch DFU magic is verified (item 6 below), the convenient
route also works: `stty -F /dev/ttyACM0 1200` → `dfu-util -a 0 -D ...`.

---

## Build status — green (2026-05-21)

Builds on the Pi with `arm-none-eabi-gcc` 8.3.1: `register_dongle.bin`,
text 34765 + data 120 B flash, bss 9624 B RAM. One fix was needed during
bring-up — `src/r_flash_lp_cfg.h`, the FSP `r_flash_lp` module config header
the Smart Configurator normally generates (the hand-made `xiao_ra4m1` board
lacks it). It is checked into the app and resolved via the `src/` include path.

FSP-integration unknowns **resolved by the green build**:

- `r_flash_lp` compiles and links exactly once — no double-compile via the RA
  `family.mk`.
- `flash_cfg_t` designated-init compiles clean — no `irq` assert.
- `R_BSP_UniqueIdGet()` / `bsp_unique_id_t.unique_id_bytes` are correct.
- The FSP linker exports `__etext`, `__data_start__/__data_end__`,
  `__bss_start__/__bss_end__`, `__StackTop/__StackLimit` — all resolve.
- The FSP umbrella header is `bsp_api.h`.

## Hardware verification (2026-05-21)

Flashed via raflash and exercised with `commission.lua` + `dongle_console.lua`
(both need `--vid-pid 2886:0053` — they default to the SAMD21 PID). Verified:

- Boot, USB-CDC enumeration, s_engine chain, libcomm framing — all CRCs ok.
- **L0 commissioning** — `commission.lua --set 1`, then `instance_id=1
  state=COMMISSIONED` survives a reboot *and* a code-flash reflash (the data
  flash is a separate region). `flash_storage.c` fully exercised.
- **Sync ladder** — `--sync` walked BOOT → L1_DONE → L2 → OPERATIONAL;
  `OP_MANIFEST_REPLY schema_hash=0x80AEB146` (matches the SAMD21 → the reused
  chain ROM is byte-correct).
- **App-shell** — `CMD_ECHO` and `CMD_SYSINFO` round-trip with `status=ok`.

Two bugs found + fixed during bring-up:

1. `r_flash_lp_cfg.h` missing — the FSP module config header (added in `src/`).
2. `flash_storage_read` read the data flash *before* `R_FLASH_LP_Open`. On the
   RA4M1 a data-flash read is reliable only after Open configures the flash
   interface — the write path opened the driver so its verify passed, but the
   boot-time commissioning load did not, so a successfully-written blob read
   back blank. Fix: `flash_storage_read` opens the driver first.

Still open — **1200-baud DFU touch** (`main.c` `DFU_DOUBLE_TAP_*`): the magic
value + RAM address are **placeholders**, so the touch resets but relaunches
the app rather than entering DFU. Flash via the BOOT-button + `raflash` route
until the values are read off the Seeed XIAO RA4M1 bootloader source.

Minor: `firmware_get_sysinfo`'s `ram_bss_b` is `__bss_end__ - __bss_start__`
(the primary `.bss`, ~3.5 KB); FSP places other zero-init regions outside it,
so treat that field as a lower bound.

---

## Source layout

```
register_dongle/
├── Makefile                     extends the blink_frame TinyUSB-make pattern
├── src/
│   ├── main.c                   USB-CDC loop, RX/TX, engine tick, reboot,
│   │                            firmware_get_sysinfo, 1200-baud DFU touch
│   ├── user_functions.c         chain user fns — per-chip copy (UID/LED/PID)
│   ├── shell_commands.{c,h}      general shell layer — reused verbatim
│   ├── ra4m1_commands.c          chip command table — NULL stub (step 4)
│   ├── flash_storage.{c,h}       RA4M1 data-flash commissioning (FSP r_flash_lp)
│   ├── r_flash_lp_cfg.h          FSP r_flash_lp module config (hand-written)
│   ├── register_dongle_v2*.{c,h} DSL-generated chain ROM — reused verbatim
│   ├── usb_descriptors.c         USB-CDC descriptors (VID 0x2886 / PID 0x0053)
│   └── tusb_config.h             TinyUSB config
└── vendor/libcomm/              vendored libcomm slice + opcodes.h
```

Reused **byte-for-byte** from the SAMD21 port: `register_dongle_v2*`,
`shell_commands.{c,h}`, `vendor/libcomm/`. The s_engine runtime (8 files)
resolves via `vpath` from `s_engine/runtime/` — no copy.
