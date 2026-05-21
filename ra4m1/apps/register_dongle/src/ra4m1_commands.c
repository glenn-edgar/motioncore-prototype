// ============================================================================
// ra4m1_commands.c — RA4M1-specific app-shell command table.
//
// Step 3b (this commit): NULL stub. register_dongle dispatches the GENERAL
// shell layer only — CMD_ECHO and CMD_SYSINFO from shell_commands.c.
// shell_find_cmd() searches the general table, then falls through to
// chip_commands_table(); returning NULL / 0 is the documented "no chip
// commands" state (see shell_commands.h).
//
// Step 4 fills g_chip_commands[] with the RA4M1 analytical-HIL command set
// (ADC / DAC / PWM / quadrature encoder). The chip plug-in contract is the
// two symbols below — step 4 only grows the table, no wiring changes.
// See memory ra4m1_pin_map for the planned command set + pin allocation.
// ============================================================================

#include "shell_commands.h"

const shell_cmd_entry_t* chip_commands_table(void) {
    return 0;   // no chip-specific commands in step 3b
}

uint8_t chip_commands_count(void) {
    return 0;
}
