# ============================================================================
# blink_frame — Seeed XIAO RA4M1 — libcomm s2m framing proof-of-life
#
# blink_engine's s_engine M-port build, extended with the vendored libcomm
# framing slice (vendor/libcomm/frame.c — SLIP + CRC-8/AUTOSAR, chip-agnostic).
# The 1 Hz heartbeat is an s2m frame instead of a text line.
#
#   make BOARD=xiao_ra4m1
#   → _build/xiao_ra4m1/blink_frame.bin   (raflash per ra4m1 notes)
# ============================================================================

TINYUSB    := ../../vendor/tinyusb
SENGINE_RT := $(abspath $(CURDIR)/../../../s_engine/runtime)
LIBCOMM    := $(CURDIR)/vendor/libcomm

include $(TINYUSB)/examples/build_system/make/make.mk

INC += \
  $(CURDIR)/src \
  $(SENGINE_RT) \
  $(LIBCOMM) \
  $(TOP)/hw \

# App sources (./src) + s_engine M-port runtime subset + vendored libcomm
# framing slice (frame.c).
SRC_C += \
  main.c \
  user_functions.c \
  usb_descriptors.c \
  blink_engine_module_rom.c \
  frame.c \
  s_engine_module.c \
  s_engine_eval.c \
  s_engine_node.c \
  s_engine_rom_init.c \
  s_engine_exception.c \
  s_engine_event_queue.c \
  se_dict_hash.c \
  se_dict_string.c \

# See blink_engine/Makefile — TinyUSB's -Werror should not gate our code.
CFLAGS += -Wno-error

include $(TINYUSB)/examples/build_system/make/rules.mk

# Bare SRC_C names resolve via vpath: app sources, s_engine runtime, libcomm.
vpath %.c $(CURDIR)/src $(SENGINE_RT) $(LIBCOMM)
