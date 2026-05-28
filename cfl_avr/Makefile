# cfl_avr — Linux-native test build.
#
# Verifies the engine logic on x86_64 before any AVR cross-compile.
# The AVR build will be a separate Makefile target using avr-gcc + the
# AVR32SD32 toolchain.

CC      = gcc
CFLAGS  = -std=c11 -Os -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS =

RUNTIME_SRC = runtime/cfl_engine.c

BUILD_DIR = build

HEARTBEAT_BIN = $(BUILD_DIR)/heartbeat_test
HEARTBEAT_SRC = tests/heartbeat/main.c tests/heartbeat/heartbeat_module.c

STATE_MACHINE_BIN = $(BUILD_DIR)/state_machine_test
STATE_MACHINE_SRC = tests/state_machine/main.c tests/state_machine/state_machine_module.c

WHILE_VERIFY_BIN = $(BUILD_DIR)/while_verify_test
WHILE_VERIFY_SRC = tests/while_verify/main.c tests/while_verify/while_verify_module.c

INIT_TERM_BIN = $(BUILD_DIR)/init_term_test
INIT_TERM_SRC = tests/init_term/main.c tests/init_term/init_term_module.c

ISR_EVENTS_BIN = $(BUILD_DIR)/isr_events_test
ISR_EVENTS_SRC = tests/isr_events/main.c tests/isr_events/isr_events_module.c

.PHONY: all clean test

all: $(HEARTBEAT_BIN) $(STATE_MACHINE_BIN) $(WHILE_VERIFY_BIN) $(INIT_TERM_BIN) $(ISR_EVENTS_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(HEARTBEAT_BIN): $(HEARTBEAT_SRC) $(RUNTIME_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(HEARTBEAT_SRC) $(RUNTIME_SRC)

$(STATE_MACHINE_BIN): $(STATE_MACHINE_SRC) $(RUNTIME_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(STATE_MACHINE_SRC) $(RUNTIME_SRC)

$(WHILE_VERIFY_BIN): $(WHILE_VERIFY_SRC) $(RUNTIME_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(WHILE_VERIFY_SRC) $(RUNTIME_SRC)

$(INIT_TERM_BIN): $(INIT_TERM_SRC) $(RUNTIME_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(INIT_TERM_SRC) $(RUNTIME_SRC)

$(ISR_EVENTS_BIN): $(ISR_EVENTS_SRC) $(RUNTIME_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(ISR_EVENTS_SRC) $(RUNTIME_SRC)

test: $(HEARTBEAT_BIN) $(STATE_MACHINE_BIN) $(WHILE_VERIFY_BIN) $(INIT_TERM_BIN) $(ISR_EVENTS_BIN)
	@echo "=== heartbeat ==="
	./$(HEARTBEAT_BIN)
	@echo ""
	@echo "=== state_machine ==="
	./$(STATE_MACHINE_BIN)
	@echo ""
	@echo "=== while_verify ==="
	./$(WHILE_VERIFY_BIN)
	@echo ""
	@echo "=== init_term ==="
	./$(INIT_TERM_BIN)
	@echo ""
	@echo "=== isr_events ==="
	./$(ISR_EVENTS_BIN)

clean:
	rm -rf $(BUILD_DIR)
