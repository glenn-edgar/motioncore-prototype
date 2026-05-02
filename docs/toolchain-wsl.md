# Arduino toolchain on WSL

The build side is 100% native Linux. `arduino-cli` is a single Go binary;
the ESP32 core is a downloadable package containing `xtensa-esp32-elf-gcc`,
`esptool.py`, and the Arduino-ESP32 framework (a thin C++ layer over
ESP-IDF + FreeRTOS). No Windows side needed for compile.

## Install

```bash
# arduino-cli into ~/.local/bin
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh \
  | BINDIR=~/.local/bin sh

# initial config (creates ~/.arduino15/arduino-cli.yaml)
arduino-cli config init

# point the board manager at the Espressif index
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json

# pull the ESP32 core — this is the big download (~600 MB):
# xtensa GCC toolchain + esptool + framework + variant headers
arduino-cli core update-index
arduino-cli core install esp32:esp32
```

Lives under `~/.arduino15/packages/esp32/`.

## Verify

```bash
arduino-cli core list              # esp32:esp32 X.Y.Z
arduino-cli board listall esp32    # "ESP32 Dev Module", etc.
```

## Daily commands

```bash
# compile a sketch (FQBN = fully-qualified board name)
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/arduino/<sketch>

# upload (requires USB passthrough — see below)
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 firmware/arduino/<sketch>

# serial console
arduino-cli monitor -p /dev/ttyUSB0 -c baudrate=115200

# install a library globally (or vendor it under sketch's src/ for a pinned copy)
arduino-cli lib install SCServo
```

For the Waveshare Servo Driver with ESP32, board target is
`esp32:esp32:esp32` (generic ESP32 Dev Module). Recommended build flags
per the Waveshare wiki: `PartitionScheme=huge_app`, `PSRAM=enabled`. Pass
via `--build-property`:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 \
  --build-property "build.partitions=huge_app" \
  --build-property "build.psram_enabled=true" \
  firmware/arduino/<sketch>
```

## USB passthrough (the WSL gotcha)

`arduino-cli` on WSL cannot see the ESP32 USB device by default — WSL2
does not expose Windows USB devices to Linux. Use Microsoft's
`usbipd-win` to bridge:

**Windows side, one-time:**
```powershell
winget install --interactive --exact dorssel.usbipd-win
```

**Windows side, after each plug-in (PowerShell, admin once for `bind`):**
```powershell
usbipd list                           # find the ESP32 (usually CP2102 or CH340)
usbipd bind   --busid <X-Y>           # one-time per device
usbipd attach --wsl Ubuntu --busid <X-Y>
```

**WSL side, verify:**
```bash
lsusb                                 # CP210x or CH340 entry
ls /dev/ttyUSB*                       # /dev/ttyUSB0
arduino-cli board list                # ESP32 should appear with FQBN
```

If `/dev/ttyUSB0` lacks permissions:
```bash
sudo usermod -aG dialout $USER        # then log out / log back in
```

## What you do NOT need

- Arduino IDE GUI — works via WSLg/X11 but is clunky; CLI is the better
  loop here.
- ESP-IDF — the Arduino-ESP32 core bundles a frozen IDF underneath. Pure
  ESP-IDF projects are a separate path (Phase 3+ Zephyr port avoids both).
- Anything Windows-side beyond `usbipd-win` for USB passthrough.

## Disk footprint

- arduino-cli binary: ~30 MB
- ESP32 core (toolchain + framework): ~600 MB under `~/.arduino15/`
- Per-sketch build cache: tens of MB under `/tmp/` by default
