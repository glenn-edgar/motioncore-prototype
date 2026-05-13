# hello_cdc - Seeeduino Xiao SAMD21
#
# Bare-metal, TinyUSB-as-a-library, newlib-nano.
# Builds hello_cdc.elf -> hello_cdc.bin -> hello_cdc.uf2

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

BUILD        := $(HERE)/build
TARGET       := hello_cdc

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
  -DCONF_DFLL_OVERWRITE_CALIBRATION=0

WARN := -Wall -Wextra -Wno-unused-parameter -Wno-unused-function

OPT  := -Os -ffunction-sections -fdata-sections
# LTO would be nice; disabled for now to keep build behavior predictable on
# the older arm-gcc 8.3.1 / nano.specs combo.

INC := \
  -I$(HERE) \
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

# Suppress the noise from Microchip's redundant-decls in hri headers, same as
# tinyusb's family.mk does.
CFLAGS += -Wno-error=redundant-decls

LDSCRIPT := $(BSP_BOARD)/seeeduino_xiao.ld
LDFLAGS  := $(CPU_FLAGS) \
            -T$(LDSCRIPT) \
            -nostartfiles \
            --specs=nano.specs --specs=nosys.specs \
            -Wl,--gc-sections \
            -Wl,-Map=$(BUILD)/$(TARGET).map \
            -Wl,--wrap=malloc
LIBS     := -lc -lm -lgcc -lnosys

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
APP_SRCS := \
  $(HERE)/main.c \
  $(HERE)/usb_descriptors.c \
  $(HERE)/syscalls_cdc.c

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

# Object files mirror source tree under build/, with app sources kept distinct.
APP_OBJS    := $(patsubst $(HERE)/%.c,$(BUILD)/app/%.o,$(APP_SRCS))
VENDOR_OBJS := $(patsubst $(TOP)/%.c,$(BUILD)/vendor/%.o,$(VENDOR_SRCS))
OBJS        := $(APP_OBJS) $(VENDOR_OBJS)

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

# Pattern rule: app sources -> build/app/...
$(BUILD)/app/%.o: $(HERE)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Pattern rule: vendor/SDK/TinyUSB sources -> build/vendor/<rel-to-TOP>/...
$(BUILD)/vendor/%.o: $(TOP)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(BUILD)

-include $(DEPS)
