-- lib/dongle_config.lua — load the per-dongle config glob (shared, build + run).
--
-- One JSON file per dongle in configs/. The supervisor's deployment shape IS the
-- set of files in that directory (Idea A): build.lua globs it to emit the IR, and
-- main.lua globs the SAME set to register each dongle's runtime — so "drop a
-- config + rebuild + restart = reconfigured." Both callers must see identical
-- bring_up_index values, which is why the index is derived here (sorted filename
-- order) rather than written into the file — you can't typo a duplicate or a gap.
--
-- Schema (configs/<dongle_id>.json):
--   { "dongle_id": "samd21-bc-1",   -- REQUIRED, unique; the runtime key
--     "device":    "/dev/ttyACM0",  -- REQUIRED, pinned (no scanning — see note)
--     "class":     "samd21_hil",    -- default "samd21_hil"
--     "instance":  "1",             -- default "1"
--     "addr":      1,               -- default 1  (RS-485 / internal routing addr)
--     "roster":    "rosters/bench.conf" }  -- default "rosters/bench.conf"
--
-- device is REQUIRED on purpose: a pinned device + the C-core flock is what makes
-- independent one_for_one restart safe (a restarted dongle can only ever re-open
-- ITS device, never wander onto a live peer's bus). Auto-scan (device=nil) is a
-- single-dongle convenience only and is rejected here.

local cjson = require("cjson")

local M = {}

-- sorted list of configs/*.json absolute paths (deterministic order = gate order)
function M.list_files(dir)
    local files = {}
    local p = io.popen('ls -1 "' .. dir .. '"/*.json 2>/dev/null')
    if p then
        for line in p:lines() do files[#files + 1] = line end
        p:close()
    end
    table.sort(files)
    return files
end

-- returns an array of validated config tables, each stamped with bring_up_index
-- (1..N by sorted filename). Errors loudly on a malformed or under-specified file.
function M.load(dir)
    local out, seen = {}, {}
    for i, path in ipairs(M.list_files(dir)) do
        local f = assert(io.open(path, "r"), "cannot open " .. path)
        local txt = f:read("*a"); f:close()
        local ok, cfg = pcall(cjson.decode, txt)
        assert(ok and type(cfg) == "table", path .. ": invalid JSON")
        assert(cfg.dongle_id and cfg.dongle_id ~= "", path .. ": missing dongle_id")
        assert(cfg.device and cfg.device ~= "", path .. ": missing device (pinning required)")
        assert(not seen[cfg.dongle_id], path .. ": duplicate dongle_id " .. tostring(cfg.dongle_id))
        seen[cfg.dongle_id] = true
        cfg.class    = cfg.class    or "samd21_hil"
        cfg.instance = tostring(cfg.instance or "1")
        cfg.addr     = tonumber(cfg.addr or 1)
        cfg.roster   = cfg.roster   or "rosters/bench.conf"
        cfg.bring_up_index = i
        out[#out + 1] = cfg
    end
    return out
end

return M
