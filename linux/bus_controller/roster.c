// roster.c — see roster.h.

#include "roster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char *errbuf, size_t cap, const char *msg, int line) {
    if (errbuf && cap) snprintf(errbuf, cap, "line %d: %s", line, msg);
    return -1;
}

int roster_load_file(const char *path, roster_t *out, char *errbuf, size_t errcap) {
    if (!path || !out) return -1;
    memset(out, 0, sizeof *out);

    FILE *f = fopen(path, "r");
    if (!f) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "cannot open '%s'", path);
        return -1;
    }

    char line[256];
    int  lineno = 0;
    int  rc = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        // strip comment + trailing newline
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char kw[32];
        char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '\0') continue;                  // blank/comment-only

        if (sscanf(p, "%31s", kw) != 1) continue;

        if (strcmp(kw, "poll") == 0) {
            unsigned period, misses, retries;
            if (sscanf(p, "%*s %u %u %u", &period, &misses, &retries) != 3)
                { rc = fail(errbuf, errcap, "poll needs <period_ms> <max_misses> <tcp_retries>", lineno); break; }
            if (period == 0 || period > 0xFFFF || misses == 0 || misses > 0xFF || retries > 0xFF)
                { rc = fail(errbuf, errcap, "poll value out of range", lineno); break; }
            out->poll_cfg_set   = 1;
            out->poll_period_ms = (uint16_t)period;
            out->max_misses     = (uint8_t)misses;
            out->tcp_retries    = (uint8_t)retries;
        } else if (strcmp(kw, "slave") == 0) {
            // strtoul with base 0 to accept 0x.. and decimal.
            char a[64], cid[64], fl[64];
            if (sscanf(p, "%*s %63s %63s %63s", a, cid, fl) != 3)
                { rc = fail(errbuf, errcap, "slave needs <addr> <class_id> <flags>", lineno); break; }
            unsigned long addr  = strtoul(a,   NULL, 0);
            unsigned long clsid = strtoul(cid, NULL, 0);
            unsigned long flags = strtoul(fl,  NULL, 0);
            if (addr == 0 || addr > 254)
                { rc = fail(errbuf, errcap, "slave addr must be 1..254", lineno); break; }
            if (flags > 0xFF)
                { rc = fail(errbuf, errcap, "slave flags must be 0..255", lineno); break; }
            if (out->count >= ROSTER_MAX_SLAVES)
                { rc = fail(errbuf, errcap, "too many slaves (max 16)", lineno); break; }
            // reject duplicate addr
            for (int i = 0; i < out->count; i++)
                if (out->slaves[i].addr == (uint8_t)addr)
                    { rc = fail(errbuf, errcap, "duplicate slave addr", lineno); break; }
            if (rc) break;
            roster_slave_t *s = &out->slaves[out->count++];
            s->addr     = (uint8_t)addr;
            s->class_id = (uint32_t)clsid;
            s->flags    = (uint8_t)flags;
        } else {
            rc = fail(errbuf, errcap, "unknown keyword (expected 'poll' or 'slave')", lineno);
            break;
        }
    }

    fclose(f);
    return rc;
}
