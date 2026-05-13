# blink_frame - Seeeduino Xiao SAMD21 (libcomm s2m framing proof-of-life)
#
# Same engine + bump allocator + TinyUSB stack as blink_engine, but the
# 1 Hz host-visible heartbeat is now a SLIP+CRC-8/AUTOSAR frame instead of
# a printf line. Vendored libcomm framing code lives under
# vendor/libcomm/ (copy of canonical libcomm; see VENDORED.md).
# Builds blink_frame.elf -> blink_frame.bin -> blink_frame.uf2

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
HERE         := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
TOP          := $(abspath $(HERE)/../..)
TUSB         := $(TOP)/vendor/tinyusb
SRC          := $(TUSB)/src
SDK          := $(TUSB)/hw/mcu/microchip/samd21
BSP_FAMILY   := $(TUSB)/hw/bsp/samd21
BSP_BOARD    := $(BSP_FAMILY)/boards/seeeduino_xiao
CMSIS        := $(TUSB)/lib/CMSIS_5/CMSIS/Core/Include
UF2CONV      := $(TUSB)/tools/uf2/utils/uf2conv.py

# s_engine runtime lives outside the samd21 tree.
SENGINE_RT   := $(abspath $(TOP)/../s_engine/runtime)

BUILD        := $(HERE)/build
TARGET       := blink_frame

# Vendored libcomm framing slice (copy of canonical libcomm; see VENDORED.md).
LIBCOMM      := $(HERE)/vendor/libcomm

# ---------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

# ---------------------------------------------------------------------------
# Flags
# ---------------------------------------------------------------------------
CPU_FLAGS := -mcpu=cortex-m0plus -mthumb

DEFS := \
  -D__SAMD21G18A__ \
  -DCFG_TUSB_MCU=OPT_MCU_SAMD21 \
  -DCONF_DFLL_OVERWRITE_CALIBRATION=0 \
  -DMODULE_IS_64BIT=0

WARN := -Wall -Wextra -Wno-unused-parameter -Wno-unused-function

OPT  := -Os -ffunction-sections -fdata-sections

INC := \
  -I$(HERE) \
  -I$(LIBCOMM) \
  -I$(SENGINE_RT) \
  -I$(SRC) \
  -I$(TUSB)/hw \
  -I$(BSP_FAMILY) \
  -I$(BSP_BOARD) \
  -I$(SDK) \
  -I$(SDK)/config \
  -I$(SDK)/include \
  -I$(SDK)/hal/include \
  -I$(SDK)/hal/utils/include \
  -I$(SDK)/hpl/pm \
  -I$(SDK)/hpl/port \
  -I$(SDK)/hri \
  -I$(CMSIS)

CFLAGS  := $(CPU_FLAGS) $(OPT) $(WARN) $(DEFS) $(INC) -std=gnu11
ASFLAGS := $(CPU_FLAGS) $(OPT) $(DEFS) $(INC)

# Microchip hri headers re-declare some symbols; same suppression hello_cdc uses.
CFLAGS += -Wno-error=redundant-decls

LDSCRIPT := $(BSP_BOARD)/seeeduino_xiao.ld
LDFLAGS  := $(CPU_FLAGS) \
            -T$(LDSCRIPT) \
            -nostartfiles \
            --specs=nano.specs --specs=nosys.specs \
            -Wl,--gc-sections \
            -Wl,--defsym=__stack_size__=0x800 \
            -Wl,-Map=$(BUILD)/$(TARGET).map
LIBS     := -lc -lm -lgcc -lnosys

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
APP_SRCS := \
  $(HERE)/main.c \
  $(HERE)/usb_descriptors.c \
  $(HERE)/user_functions.c \
  $(HERE)/blink_engine_module_rom.c

LIBCOMM_SRCS := \
  $(LIBCOMM)/frame.c

# M-port engine subset. Explicitly excludes:
#   s_engine_loader.c   - no blob loader
#   s_engine_init.c     - printf-diagnostic path; we use s_engine_init_rom
#   s_engine_builtins.c - global registration table; emit-c uses headers directly
#   s_engine_stack.c    - no stack on M-port
#   *.cc                - C++ helpers not needed for the M-port subset
SENGINE_SRCS := \
  $(SENGINE_RT)/s_engine_module.c \
  $(SENGINE_RT)/s_engine_eval.c \
  $(SENGINE_RT)/s_engine_node.c \
  $(SENGINE_RT)/s_engine_rom_init.c \
  $(SENGINE_RT)/s_engine_exception.c \
  $(SENGINE_RT)/s_engine_event_queue.c \
  $(SENGINE_RT)/se_dict_hash.c \
  $(SENGINE_RT)/se_dict_string.c

BSP_SRCS := \
  $(BSP_FAMILY)/family.c \
  $(SDK)/gcc/system_samd21.c \
  $(SDK)/gcc/gcc/startup_samd21.c \
  $(SDK)/hal/src/hal_atomic.c \
  $(SDK)/hpl/gclk/hpl_gclk.c \
  $(SDK)/hpl/pm/hpl_pm.c \
  $(SDK)/hpl/sysctrl/hpl_sysctrl.c

TUSB_SRCS := \
  $(SRC)/tusb.c \
  $(SRC)/common/tusb_fifo.c \
  $(SRC)/device/usbd.c \
  $(SRC)/device/usbd_control.c \
  $(SRC)/class/cdc/cdc_device.c \
  $(SRC)/portable/microchip/samd/dcd_samd.c

VENDOR_SRCS := $(BSP_SRCS) $(TUSB_SRCS)

APP_OBJS     := $(patsubst $(HERE)/%.c,$(BUILD)/app/%.o,$(APP_SRCS))
VENDOR_OBJS  := $(patsubst $(TOP)/%.c,$(BUILD)/vendor/%.o,$(VENDOR_SRCS))
SENGINE_OBJS := $(patsubst $(SENGINE_RT)/%.c,$(BUILD)/sengine/%.o,$(SENGINE_SRCS))
LIBCOMM_OBJS := $(patsubst $(LIBCOMM)/%.c,$(BUILD)/libcomm/%.o,$(LIBCOMM_SRCS))
OBJS         := $(APP_OBJS) $(VENDOR_OBJS) $(SENGINE_OBJS) $(LIBCOMM_OBJS)

DEPS := $(OBJS:.o=.d)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
.PHONY: all clean size uf2

all: $(BUILD)/$(TARGET).uf2 size

$(BUILD)/$(TARGET).elf: $(OBJS) $(LDSCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/$(TARGET).uf2: $(BUILD)/$(TARGET).bin
	python3 $(UF2CONV) -c -b 0x2000 -f 0x68ed2b88 -o $@ $<

size: $(BUILD)/$(TARGET).elf
	@$(SIZE) $<

# App sources -> build/app/...
$(BUILD)/app/%.o: $(HERE)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Vendor/SDK/TinyUSB sources -> build/vendor/...
$(BUILD)/vendor/%.o: $(TOP)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# s_engine runtime sources -> build/sengine/...
$(BUILD)/sengine/%.o: $(SENGINE_RT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Vendored libcomm sources -> build/libcomm/...
$(BUILD)/libcomm/%.o: $(LIBCOMM)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(BUILD)

-include $(DEPS)
