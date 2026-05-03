# Dev-host setup — Raspberry Pi 4 as build target for the Pico 2 W

This repo is developed using a **Raspberry Pi 4 as the dev host**: Pico SDK
toolchain, FreeRTOS-Kernel, picotool, and all builds live on the Pi. Edit
from any machine via SSHFS; build over SSH. The eventual robot host is also
Linux, so the build environment matches the deployment environment.

This doc records the actual setup steps that produced a working build of
`firmware/pico/apps/00_smp_hello/` on 2026-05-03. Adapt as needed.

## Why this layout

- The robot's USB-TTY adapters live on the Linux host; serial/parser code
  must be Linux-POSIX. Develop where you deploy.
- Avoids `usbipd-win` per-board registration pain when you have many boards.
- Pi SDK toolchain installs cleanly on Raspberry Pi OS aarch64 (with two
  caveats — see §3 below).
- WSL becomes editor-only via SSHFS; no driver fuss for flashing.

## 1. Prerequisites

- A Raspberry Pi (4 or 5) with Raspberry Pi OS or Debian, aarch64, ≥4 GB RAM,
  ≥8 GB free disk.
- The Pi reachable on your LAN with `ssh` enabled.
- A working `sudo` for the Pi user (passwordless preferred).
- An editor host (WSL, macOS, Linux) that can run `ssh` + `sshfs`.

## 2. SSH alias + SSHFS mount (on the editor host)

```bash
# 2a. Generate a dedicated key for this Pi (recommended over reusing one).
ssh-keygen -t ed25519 -f ~/.ssh/id_pi4_66 -C "you@host→pi@<ip>"

# 2b. Copy the public key to the Pi (one-time interactive password prompt).
ssh-copy-id -i ~/.ssh/id_pi4_66.pub pi@<ip>
```

Add to `~/.ssh/config`:

```
Host robot
    HostName 192.168.1.66
    User pi
    IdentityFile ~/.ssh/id_pi4_66
    IdentitiesOnly yes
    ServerAliveInterval 30
    ServerAliveCountMax 3
```

`chmod 600 ~/.ssh/config`. Verify with `ssh robot uname -a`.

Mount the Pi's home over SSHFS:

```bash
mkdir -p ~/robot-fs
sshfs robot:/home/pi ~/robot-fs \
  -o reconnect,ServerAliveInterval=15,ServerAliveCountMax=3
```

The Pi's home tree now appears at `~/robot-fs/`. Edit files there with any
editor — changes land on the Pi instantly. **Build over SSH, not through
SSHFS** (CMake/Ninja get confused by SSHFS file timestamps).

If the mount goes stale after laptop sleep:

```bash
fusermount -u ~/robot-fs
sshfs robot:/home/pi ~/robot-fs -o reconnect,ServerAliveInterval=15
```

## 3. Pi-side toolchain install

### 3.1 apt prerequisites

```bash
ssh robot 'sudo apt-get update && sudo apt-get install -y \
  ninja-build build-essential libusb-1.0-0-dev pkg-config \
  python3-pip wget xz-utils git'
```

### 3.2 Newer CMake (Bullseye apt is too old)

Bullseye ships CMake 3.18; pico-sdk needs 3.19+ for the `pico_status_led`
target (`INTERFACE_LIBRARY ... LINK_LIBRARIES` query). Get a current CMake
via pip — no sudo, no system overwrite:

```bash
ssh robot 'pip3 install --user --upgrade cmake'
# Adds ~/.local/bin/cmake — must precede /usr/bin/cmake on PATH.
```

### 3.3 Arm GNU Toolchain (apt's gcc-arm-none-eabi is gcc 8 — unusable)

The Bullseye apt `gcc-arm-none-eabi` is gcc 8 from 2019. It does not
support RP2350 (Cortex-M33) cleanly. Always install Arm's binary
toolchain instead. **If your Pi's outbound internet is slow** (some
networks throttle Arm's CDN), download on a faster machine and rsync
over LAN:

```bash
# On the editor host:
wget https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-aarch64-arm-none-eabi.tar.xz
rsync -av arm-gnu-toolchain-*.tar.xz robot:/home/pi/pico/

# On the Pi:
ssh robot 'cd ~/pico && tar -xf arm-gnu-toolchain-*.tar.xz && \
           rm arm-gnu-toolchain-*.tar.xz'
```

(Or just `wget` directly on the Pi if your network is fine.)

### 3.4 pico-sdk + pico-extras + pico-examples

```bash
ssh robot 'mkdir -p ~/pico && cd ~/pico && \
  git clone --depth 1 -b master https://github.com/raspberrypi/pico-sdk.git && \
  ( cd pico-sdk && git submodule update --init --depth 1 ) && \
  git clone --depth 1 https://github.com/raspberrypi/pico-extras.git && \
  git clone --depth 1 https://github.com/raspberrypi/pico-examples.git'
```

The pico-sdk submodules (cyw43-driver, lwip, mbedtls, tinyusb, btstack)
total ~340 MB. With `--depth 1` they pin to branch tips, which usually
works but may differ from pico-sdk's exact pinned commits — drop `--depth
1` if you hit any submodule-version compatibility issue.

### 3.5 FreeRTOS-Kernel — **MUST** be the Raspberry Pi fork

**Mainline `FreeRTOS/FreeRTOS-Kernel` does NOT contain the RP2350 ports.**
The Raspberry Pi fork at <https://github.com/raspberrypi/FreeRTOS-Kernel>
has them at `portable/ThirdParty/GCC/RP2350_ARM_NTZ` and `RP2350_RISC-V`.

```bash
ssh robot 'cd ~/pico && \
  git clone --depth 1 https://github.com/raspberrypi/FreeRTOS-Kernel.git && \
  ( cd FreeRTOS-Kernel && git submodule update --init --depth 1 )'
```

### 3.6 picotool (build from source — apt's is too old)

```bash
ssh robot 'cd ~/pico && \
  git clone https://github.com/raspberrypi/picotool.git && \
  cd picotool && mkdir build && cd build && \
  PICO_SDK_PATH=$HOME/pico/pico-sdk cmake .. && \
  make -j$(nproc) && sudo make install'
# Installs to /usr/local/bin/picotool.
```

### 3.7 Environment variables (bashrc)

Append to `~/.bashrc` on the Pi:

```bash
export PATH="$HOME/.local/bin:$PATH"   # newer cmake
export PICO_SDK_PATH="$HOME/pico/pico-sdk"
export PICO_EXTRAS_PATH="$HOME/pico/pico-extras"
export FREERTOS_KERNEL_PATH="$HOME/pico/FreeRTOS-Kernel"
export PATH="$HOME/pico/arm-gnu-toolchain-14.2.rel1-aarch64-arm-none-eabi/bin:$PATH"
```

**Caveat:** non-interactive ssh sessions skip `~/.bashrc`. When
scripting builds via `ssh robot 'cmd'`, either pass env vars inline or
use `bash -lc 'cmd'` to force a login shell.

## 4. Verification

A fresh login shell should give:

```bash
ssh robot
which cmake          # → /home/pi/.local/bin/cmake
cmake --version      # → 4.x
arm-none-eabi-gcc --version  # → 14.2.x
picotool version     # → 2.2.0-a4 or newer
```

End-to-end build smoke test (skips FreeRTOS examples, see §5 below):

```bash
ssh robot 'cd ~/pico/pico-examples && mkdir -p build && cd build && \
  unset FREERTOS_KERNEL_PATH; \
  cmake -DPICO_BOARD=pico2_w -DPICO_PLATFORM=rp2350-arm-s -G Ninja .. && \
  ninja hello_serial && \
  ls -la hello_world/serial/hello_serial.uf2'
```

You should see a `hello_serial.uf2` (~16 KB) ready to flash.

## 5. Known gotchas

1. **pico-examples' `FreeRTOS_Kernel_import.cmake` files** are incompatible
   with the RPi FreeRTOS-Kernel fork — they hard-code the mainline layout
   (`portable/ThirdParty/Community-Supported-Ports/...`) which the RPi fork
   doesn't have. To smoke-test pico-examples, configure with
   `FREERTOS_KERNEL_PATH` unset so the FreeRTOS examples get skipped.
   **Our own apps** include the FreeRTOS_Kernel_import.cmake that ships
   *inside the RPi fork* itself — see
   `firmware/pico/apps/00_smp_hello/CMakeLists.txt` for the pattern.

2. **`FreeRTOSConfig.h` for the RP2350 ARM-NTZ SMP port needs more knobs
   than typical FreeRTOS configs.** See
   `firmware/pico/apps/00_smp_hello/FreeRTOSConfig.h` — must define
   `configUSE_PASSIVE_IDLE_HOOK`, `configENABLE_TRUSTZONE`,
   `configRUN_FREERTOS_SECURE_ONLY`, `configENABLE_MPU`,
   `configENABLE_FPU`, `secureconfigMAX_SECURE_CONTEXTS`, both
   `configMAX_API_CALL_INTERRUPT_PRIORITY` and
   `configMAX_SYSCALL_INTERRUPT_PRIORITY`,
   `configKERNEL_INTERRUPT_PRIORITY`, and a `configASSERT(x)` macro.
   `configASSERT` must use `portDISABLE_INTERRUPTS()` (not
   `taskDISABLE_INTERRUPTS()`) because portmacro.h calls it before task.h
   is included.

3. **Static-allocation + SMP requires four user callbacks**:
   `vApplicationGetIdleTaskMemory`,
   `vApplicationGetPassiveIdleTaskMemory(..., BaseType_t xCoreID)` (note
   the SMP-specific signature with the core-ID parameter — one passive
   idle task per non-main core), `vApplicationGetTimerTaskMemory`, and
   (with `configUSE_MALLOC_FAILED_HOOK=1`) `vApplicationMallocFailedHook`.
   Minimal versions are in `00_smp_hello/main.c`.

4. **Flashing without USB drivers:** drop the `.uf2` onto the
   `RPI-RP2`/`RP2350` mass-storage drive that appears when the Pico is
   booted with BOOTSEL held. No usbipd, no `picotool` invocation needed.

## 6. Repo layout convention

- `~/pico/` on the Pi: toolchain + SDKs (NOT in this repo).
- `~/work/motioncore-prototype/` on the Pi: this repo's clone.
- Each app under `firmware/pico/apps/<NN_name>/` is a standalone CMake
  project — its own `CMakeLists.txt`, `FreeRTOSConfig.h`, `main.c`. To
  build:
  ```bash
  ssh robot 'cd ~/work/motioncore-prototype/firmware/pico/apps/<APP> && \
             mkdir -p build && cd build && cmake -G Ninja .. && ninja'
  ```
  (Plus the env-var preamble if your shell didn't pick up bashrc.)
