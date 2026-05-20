# ============================================================================
# hello_cdc — Seeed XIAO RA4M1 USB-CDC bring-up
#
# Builds against the vendored TinyUSB + Renesas FSP using TinyUSB's own make
# build system. App sources live here in src/ (git-tracked); the vendored
# TinyUSB tree is separate (and gitignored).
#
#   make BOARD=xiao_ra4m1
#   → _build/xiao_ra4m1/hello_cdc.bin   (DFU-flash per README)
#
# TinyUSB's build resolves sources via `vpath %.c . $(TOP)` (set in rules.mk);
# we append our src/ dir to vpath so the bare SRC_C names below resolve, which
# lets the app live outside the vendored tree.
# ============================================================================

TINYUSB := ../../vendor/tinyusb

include $(TINYUSB)/examples/build_system/make/make.mk

INC += \
  $(CURDIR)/src \
  $(TOP)/hw \

SRC_C += \
  main.c \
  usb_descriptors.c \

include $(TINYUSB)/examples/build_system/make/rules.mk

# Our app sources live in ./src — append to the vpath rules.mk set up.
vpath %.c $(CURDIR)/src
