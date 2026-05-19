-- lib/clock.lua — POSIX clock readers for fake_robot.
--
-- Two clocks for two jobs:
--   now_ms()   — CLOCK_MONOTONIC, integer milliseconds. For elapsed-time
--                math (timeouts, backoff, publish cadence). Immune to
--                wall-clock jumps (NTP step, manual set, VM pause/resume).
--   wall_now() — CLOCK_REALTIME + gmtime breakout. For calendar-anchored
--                scheduling and ts fields published on the wire. Field
--                shape matches the C system's cfl_time_info_t.
--
-- Lua doubles (53-bit mantissa) hold int64 ms with no precision loss for
-- ~285k years and epoch seconds with sub-ns precision until year 2554.

local ffi = require("ffi")

-- ct_builtins cdef's these too; pcall keeps us safe in either load order.
pcall(ffi.cdef, "typedef struct { long tv_sec; long tv_nsec; } ct_log_timespec_t;")
pcall(ffi.cdef, "int clock_gettime(int clk_id, ct_log_timespec_t *tp);")

local _ts = ffi.new("ct_log_timespec_t")
local CLOCK_REALTIME  = 0
local CLOCK_MONOTONIC = 1

local M = {}

function M.now_ms()
    ffi.C.clock_gettime(CLOCK_MONOTONIC, _ts)
    return tonumber(_ts.tv_sec) * 1000
         + math.floor(tonumber(_ts.tv_nsec) / 1e6)
end

-- Returns a table with the same field shape as the C cfl_time_info_t:
--   { year, month, day, dow (Mon=0..Sun=6), doy (1-366),
--     hour, minute, second, epoch_s (fractional double) }
-- All UTC.
function M.wall_now()
    ffi.C.clock_gettime(CLOCK_REALTIME, _ts)
    local sec  = tonumber(_ts.tv_sec)
    local nsec = tonumber(_ts.tv_nsec)
    local t    = os.date("!*t", sec)
    return {
        year    = t.year,
        month   = t.month,
        day     = t.day,
        dow     = (t.wday + 5) % 7,   -- Lua wday Sun=1..Sat=7 → Mon=0..Sun=6
        doy     = t.yday,
        hour    = t.hour,
        minute  = t.min,
        second  = t.sec,
        epoch_s = sec + nsec * 1e-9,
    }
end

return M
