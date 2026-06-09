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

# mon_stream — enable CMD_MON_STREAM and count the periodic KB0 report cycles.
mon_stream: mon_stream_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# mon_adc — drive CMD_ADC_READ to KB1 (api/HIL) on the core1 engine; read ADC0 (GP26).
mon_adc: mon_adc_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# dac_adc_xcheck — sweep the slave DAC (A0) and read it on the Pico ADC0 (jumpered).
dac_adc_xcheck: dac_adc_xcheck_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# mon_adc_stats — sweep slave DAC, read the Pico 10 Hz mean/max/rms streams (CMD_ADC_STATS).
mon_adc_stats: mon_adc_stats_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# mon_gpio — GPIO loopback + 1 kHz pulse-count test on the Pico BC (GP2->GP3 jumper).
mon_gpio: mon_gpio_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# mon_servo — drive the servo bank; verify the 50 Hz frame rate via GP3 counter (GP2->GP3).
mon_servo: mon_servo_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# mon_pwm — verify GP14 PWM duty via ADC0 mean (GP14->GP26 jumper).
mon_pwm: mon_pwm_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# mon_quad — verify GP17/18 quad decoder with a software fake encoder (GP2->GP17, GP3->GP18).
mon_quad: mon_quad_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# mon_i2c — scan the I2C manager bus + optional register read (i2c1 GP10/11).
mon_i2c: mon_i2c_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# mon_servo_stop — verify servo mode terminates and the pin returns to GPIO (no jumper).
mon_servo_stop: mon_servo_stop_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# mon_samd21 — exercise the SAMD21 I2C client control bank (WHO_AM_I/UNIQUE_ID/MODE).
mon_samd21: mon_samd21_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# mon_pio — exercise SAMD21 PIO mode (8-bit GPIO expander: IODIR/OLAT/GPIO).
mon_pio: mon_pio_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
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

# mon_pio_b — exercise SAMD21 PIO-mode gpio interlock (DSL trip + INT + manual reset).
mon_pio_b: mon_pio_b_main.c controller.c demux.c identity.c roster.c cmd_tracker.c usb_link.c $(LIBCOMM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
