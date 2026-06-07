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
.PHONY: all clean step0a step0b step1 step2 step3 step4 step5 step6a step6b step7a step7b1 step7b2 step7b3 capstone
all: step0a step0b step1 step2 step3 step4 step5 step6a step6b step7a step7b1 step7b2 step7b3 capstone bench

# Step 0a — link-endpoint seam + USB link manager bring-up driver.
step0a: step0a_main.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# mon_snapshot — drive CMD_MON_SNAPSHOT to the core1 app engine (0xFB), decode the
# KB0 OP_MON_* report set via the controller raw async-frame callback.
mon_snapshot: mon_snapshot_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
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

# Step 7b-2 — BC->Pi periodic status report (the reliable index; zero bus traffic).
step7b2: step7b2_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Step 7b-3 — Pi reconciliation: index vs received messages, targeted re-push.
step7b3: step7b3_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Capstone — fake console: full API suite + (Step 7) interlock arm/trigger.
capstone: capstone_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f step0a step0b step1 step2 step3 step4 step5 step6a capstone

# Bench — bus soak + API throughput benchmark.
bench: bench_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# evtest — verify the FFI event seam (submit_command_ev + drain).
evtest: evtest_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Shared lib for the LuaJIT FFI wrapper (the portable C core, position-independent).
SOLIB_SRC := ffi_facade.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
libbus_controller.so: $(SOLIB_SRC)
	$(CC) $(CFLAGS) -fPIC -shared $^ -o $@ $(LDFLAGS)
