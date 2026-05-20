# ============================================================================
# blink_engine — Seeed XIAO RA4M1 — s_engine M-port bring-up
#
# Extends the hello_cdc TinyUSB-make-system build with the s_engine M-port
# runtime (8 chip-agnostic .c files from ../../../s_engine/runtime) plus the
# generated chain ROM. blink_engine_module_rom.c and blink_engine.h are reused
# byte-for-byte from samd21/apps/blink_engine — the portability keystone:
# same chain ROM, recompiled engine, new chip layer.
#
#   make BOARD=xiao_ra4m1
#   → _build/xiao_ra4m1/blink_engine.bin   (raflash per ra4m1 notes)
# ============================================================================

TINYUSB    := ../../vendor/tinyusb
SENGINE_RT := $(abspath $(CURDIR)/../../../s_engine/runtime)

include $(TINYUSB)/examples/build_system/make/make.mk

INC += \
  $(CURDIR)/src \
  $(SENGINE_RT) \
  $(TOP)/hw \

# App sources (./src) + the s_engine M-port runtime subset. The subset
# excludes loader / init / builtins-table / stack / *.cc — the same split
# the SAMD21 blink_engine Makefile documents.
SRC_C += \
  main.c \
  user_functions.c \
  usb_descriptors.c \
  blink_engine_module_rom.c \
  s_engine_module.c \
  s_engine_eval.c \
  s_engine_node.c \
  s_engine_rom_init.c \
  s_engine_exception.c \
  s_engine_event_queue.c \
  se_dict_hash.c \
  se_dict_string.c \

# TinyUSB's RA build compiles everything with -Werror plus a strict warning
# set (-Wdouble-promotion, -Wredundant-decls, -fsingle-precision-constant, ...)
# to keep TinyUSB itself clean. Our app + the s_engine M-port runtime are
# separately maintained and build clean under their own (SAMD21) flags, so
# don't gate them on TinyUSB's -Werror. Warnings stay visible, just non-fatal.
CFLAGS += -Wno-error

include $(TINYUSB)/examples/build_system/make/rules.mk

# Bare SRC_C names resolve via vpath: app sources in ./src, s_engine runtime
# out-of-tree. Appended to the vpath rules.mk already set up.
vpath %.c $(CURDIR)/src $(SENGINE_RT)
