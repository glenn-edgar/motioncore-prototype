# Pi-side Layer-2 bus controller (Plan 1). Native host build (Pi / WSL dev host).
#
# The portable controller routine links the SAME libcomm frame codec the SAMD21
# firmware runs (vendor/libcomm), so the USB wire format matches by construction.

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=c11
CFLAGS  += -D_GNU_SOURCE -I. -Ivendor/libcomm
LDFLAGS ?=

LIBCOMM := vendor/libcomm/frame.c

# ---- targets ---------------------------------------------------------------
.PHONY: all clean step0a step0b step1
all: step0a step0b step1

# Step 0a — link-endpoint seam + USB link manager bring-up driver.
step0a: step0a_main.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Step 0b — demux reader (reply-vs-async dispatch, monotonic request_ids).
step0b: step0b_main.c demux.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Step 1 — find dongle + connect; identify role via REGISTER class_id.
step1: step1_main.c controller.c demux.c identity.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f step0a step0b step1
