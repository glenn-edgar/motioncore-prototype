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
.PHONY: all clean step0a step0b step1 step2 step3 step4 step5 step6a step6b step7a step7b1 capstone
all: step0a step0b step1 step2 step3 step4 step5 step6a step6b step7a step7b1 capstone

# Step 0a — link-endpoint seam + USB link manager bring-up driver.
step0a: step0a_main.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Step 0b — demux reader (reply-vs-async dispatch, monotonic request_ids).
step0b: step0b_main.c demux.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Step 1 — find dongle + connect; identify role via REGISTER class_id.
step1: step1_main.c controller.c demux.c identity.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Step 2 — controller-driven sync ladder + auto-resync.
step2: step2_main.c controller.c demux.c identity.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Step 3 — roster recall from disk + push sweep list to the BC via CMD_BUS_*.
step3: step3_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Step 4 — BC autonomous poll sweep + liveness escalation (point-to-point slave).
step4: step4_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Step 5 — content command->reply to a slave injected through the sweep.
step5: step5_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Step 6a — L2 command tracker: per-slave queue + availability + one-in-flight.
step6a: step6a_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Step 6b — ACK-frees-the-bus + async reply on a later poll + L2 two-timeout.
step6b: step6b_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Step 7a — L2 readable interlock buffer + CMD_SLOT_FAULTED queue gating.
step7a: step7a_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Step 7b-1 — slave buffer-2 interlock-message PUSH on a trip edge.
step7b1: step7b1_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Capstone — fake console: full API suite + (Step 7) interlock arm/trigger.
capstone: capstone_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f step0a step0b step1 step2 step3 step4 step5 step6a capstone
