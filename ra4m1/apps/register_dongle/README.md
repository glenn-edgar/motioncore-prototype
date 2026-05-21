# register_dongle — RA4M1 dongle firmware (step 3b)

The SAMD21 `register_dongle` ported to the Seeed XIAO RA4M1 (Renesas
R7FA4M1AB, Cortex-M4). Speaks the libcomm wire protocol to a Linux host over
USB-CDC, runs the four-layer sync ladder, and exposes a binary command shell.

This is the **second** chip in the four-chip dongle suite. It reuses the
engine, the chain ROM, the general shell layer, and libcomm unchanged from the
SAMD21 reference; only the chip layer (flash storage, sysinfo, UID, the DFU
touch) is RA4M1-specific.

> **Status (2026-05-21):** builds green on the Pi — `register_dongle.bin`,
> ~34 KB flash / ~9.6 KB RAM. **Not yet hardware-verified** — flashing and the
> sync-ladder walk need the bench. The FSP-integration unknowns are resolved
> except two runtime items; see "Build status" and "Verify on hardware" below.

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

## Verify on hardware

Not yet done — needs the bench. Flash, then walk the sync ladder with
`linux/dongle_console/dongle_console.lua` (see the SAMD21 README). Two items
can only be confirmed at runtime:

- **Commissioning / data flash** (`flash_storage.c`) — run an
  `OP_COMMISSION_SET` → reboot → `--status` → `OP_COMMISSION_CLEAR` cycle and
  confirm the `instance_id` persists across reboot. This exercises the
  data-flash erase/write and the real erase-block size (slots are 2 KB apart —
  safe for any block ≤ 2 KB).
- **1200-baud DFU touch** (`main.c` `DFU_DOUBLE_TAP_*`) — the magic value and
  RAM address are still **placeholders**; the touch resets but relaunches the
  app until they are read off the Seeed XIAO RA4M1 bootloader source. Flash via
  the BOOT-button + `raflash` route meanwhile.

Minor: `firmware_get_sysinfo`'s `ram_bss_b` is `__bss_end__ - __bss_start__`
(the primary `.bss`); FSP may place extra zero-init regions outside it, so
treat that figure as a lower bound.

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
