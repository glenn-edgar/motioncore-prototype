// roster.h — the Layer-2 AUTHORITATIVE roster: the controller's source of truth
// for which slaves live on the bus (address + class_id + flags) plus the poll
// config. Loaded from disk on startup ("recall"); the controller pushes the
// derived sweep list (addresses) down to the BC via CMD_BUS_*.
//
// Two rosters, deliberately distinct:
//   * Layer-2 (this)  — class_ids, flags, persistence; lives on the host.
//   * Layer-1 (BC)    — addresses + liveness; lives on the dongle's sweep engine.
//
// File format (text; '#' comments, blank lines ignored):
//   poll  <period_ms> <max_misses> <tcp_retries>
//   slave <addr> <class_id> <flags>      # numbers accept 0x.. or decimal
// e.g.
//   poll  500 3 2
//   slave 10 0x5E589100 0x03

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROSTER_MAX_SLAVES 16

// Slave flags (mirror bus_roster.h BUS_FLAG_*).
#define ROSTER_FLAG_TCP      0x01u
#define ROSTER_FLAG_ENABLED  0x02u

typedef struct {
    uint8_t  addr;        // 1..254
    uint32_t class_id;
    uint8_t  flags;
} roster_slave_t;

typedef struct {
    int            poll_cfg_set;   // 1 if a 'poll' line was present
    uint16_t       poll_period_ms;
    uint8_t        max_misses;
    uint8_t        tcp_retries;

    roster_slave_t slaves[ROSTER_MAX_SLAVES];
    int            count;
} roster_t;

// Load a roster from a text file. Returns 0 on success; -1 on open/parse error
// (a human-readable reason is written to errbuf when provided).
int roster_load_file(const char *path, roster_t *out, char *errbuf, size_t errcap);

#ifdef __cplusplus
}
#endif
