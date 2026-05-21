# register_dongle — RA4M1 dongle firmware (step 3b)

The SAMD21 `register_dongle` ported to the Seeed XIAO RA4M1 (Renesas
R7FA4M1AB, Cortex-M4). Speaks the libcomm wire protocol to a Linux host over
USB-CDC, runs the four-layer sync ladder, and exposes a binary command shell.

This is the **second** chip in the four-chip dongle suite. It reuses the
engine, the chain ROM, the general shell layer, and libcomm unchanged from the
SAMD21 reference; only the chip layer (flash storage, sysinfo, UID, the DFU
touch) is RA4M1-specific.

> **Status:** code complete, **not yet built or hardware-verified.** The build
> happens on the Pi (`ssh robot`) — the WSL checkout has no cross toolchain and
> no vendored FSP tree. Six FSP-integration details could not be confirmed from
> WSL; they ship as `TODO-verify` markers in the code. **Resolve the checklist
> below on the first Pi build.**

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

## TODO-verify checklist (first Pi build)

These could not be checked from WSL (the FSP tree lives only on the Pi). Each
is marked `TODO-verify` at its site in the code.

| # | Item | Where | What to do |
|---|------|-------|-----------|
| 1 | FSP `r_flash_lp` build wiring | `Makefile` | Confirm `r_flash_lp.c` path + `inc/api`,`inc/instances` on the include path. If the link reports **duplicate** `r_flash_lp` symbols, the RA `family.mk` already compiles it — drop `r_flash_lp.c` from `SRC_C`. |
| 2 | `flash_cfg_t` fields | `flash_storage.c` | If the FSP build asserts on an unset `irq`, set `.irq = FSP_INVALID_VECTOR`. |
| 3 | Data-flash erase-block size | `flash_storage.c` | Slots are spaced 2 KB apart — robust to any block ≤ 2 KB (every real RA4M1 geometry). Only revisit if FSP reports a larger block. |
| 4 | `R_BSP_UniqueIdGet()` accessor | `user_functions.c` | Confirm the call returns `bsp_unique_id_t*` with a `.unique_id_bytes[16]` member; adjust if the field name differs. |
| 5 | FSP linker symbols | `main.c` `firmware_get_sysinfo` | Confirm `__etext`, `__data_start__`, `__data_end__`, `__bss_start__`, `__bss_end__`, `__StackTop`, `__StackLimit` against the FSP `fsp.ld`. |
| 6 | Seeed bootloader DFU magic | `main.c` `DFU_DOUBLE_TAP_*` | The "stay in DFU" RAM address + magic value are **placeholders**. Read them off the Seeed XIAO RA4M1 bootloader source (or confirm empirically). Until then the 1200-baud touch resets but relaunches the app. |

Also confirm the FSP umbrella header is named `bsp_api.h` (included by `main.c`
and `user_functions.c` for `NVIC_SystemReset` / `SystemCoreClock` /
`R_BSP_UniqueIdGet`); adjust if the vendored FSP uses a different name.

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
│   ├── register_dongle_v2*.{c,h} DSL-generated chain ROM — reused verbatim
│   ├── usb_descriptors.c         USB-CDC descriptors (VID 0x2886 / PID 0x0053)
│   └── tusb_config.h             TinyUSB config
└── vendor/libcomm/              vendored libcomm slice + opcodes.h
```

Reused **byte-for-byte** from the SAMD21 port: `register_dongle_v2*`,
`shell_commands.{c,h}`, `vendor/libcomm/`. The s_engine runtime (8 files)
resolves via `vpath` from `s_engine/runtime/` — no copy.
