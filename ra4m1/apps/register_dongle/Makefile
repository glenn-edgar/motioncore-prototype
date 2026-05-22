# ============================================================================
# register_dongle — Seeed XIAO RA4M1 — step 3b
#
# The SAMD21 register_dongle ported to the RA4M1. Merges the s_engine M-port
# (proven on RA4M1 by blink_engine), libcomm SLIP+CRC framing (proven by
# blink_frame), and the register_dongle_v2 chain into one binary. Extends the
# blink_frame Makefile pattern with the register_dongle sources + the FSP
# low-power flash driver (data-flash commissioning storage).
#
#   make BOARD=xiao_ra4m1
#   -> _build/xiao_ra4m1/register_dongle.bin   (flash per ra4m1_bringup notes)
# ============================================================================

TINYUSB    := ../../vendor/tinyusb
SENGINE_RT := $(abspath $(CURDIR)/../../../s_engine/runtime)
LIBCOMM    := $(CURDIR)/vendor/libcomm

include $(TINYUSB)/examples/build_system/make/make.mk

# FSP low-power flash driver — drives the data flash for commissioning storage.
# TODO-verify (Pi): confirm these paths against the vendored FSP tree. The
# inc/api + inc/instances dirs are usually already on the include path via the
# RA family.mk (the rusb2 USB driver needs them); listed here defensively.
# If the link reports duplicate r_flash_lp symbols, the family.mk already
# compiles it — drop r_flash_lp.c from SRC_C below.
FSP_FLASH_SRC := $(TOP)/hw/mcu/renesas/fsp/ra/fsp/src/r_flash_lp
FSP_INC       := $(TOP)/hw/mcu/renesas/fsp/ra/fsp/inc

# $(CURDIR) is on the include path so shell_commands.c's
# #include "vendor/libcomm/opcodes.h" resolves (the file is reused verbatim
# from the SAMD21 port, which has a flat app dir).
INC += \
  $(CURDIR)/src \
  $(CURDIR) \
  $(SENGINE_RT) \
  $(LIBCOMM) \
  $(TOP)/hw \
  $(FSP_INC)/api \
  $(FSP_INC)/instances \

# App sources (./src) + register_dongle_v2 chain ROM + vendored libcomm
# framing slice (frame.c) + s_engine M-port runtime subset + FSP flash driver.
SRC_C += \
  main.c \
  user_functions.c \
  usb_descriptors.c \
  flash_storage.c \
  shell_commands.c \
  ra4m1_commands.c \
  ra4m1_hal.c \
  mode.c \
  register_dongle_v2_module_rom.c \
  frame.c \
  s_engine_module.c \
  s_engine_eval.c \
  s_engine_node.c \
  s_engine_rom_init.c \
  s_engine_exception.c \
  s_engine_event_queue.c \
  se_dict_hash.c \
  se_dict_string.c \
  r_flash_lp.c \

# See blink_engine/Makefile — TinyUSB's strict -Werror should not gate our
# separately-maintained app + engine + libcomm code.
CFLAGS += -Wno-error

include $(TINYUSB)/examples/build_system/make/rules.mk

# Bare SRC_C names resolve via vpath: app sources + chain ROM (src/),
# s_engine runtime, vendored libcomm, FSP flash driver.
vpath %.c $(CURDIR)/src $(SENGINE_RT) $(LIBCOMM) $(FSP_FLASH_SRC)
