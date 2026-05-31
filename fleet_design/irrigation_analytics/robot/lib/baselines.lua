-- baselines.lua — loader for baselines.json (curve generator output).
--
-- Single-file consumer: read once at startup, return a table keyed by
-- bin_key for O(1) lookup. Robot side compares per-bin reductions or
-- live samples against the stored ref + thresholds.
--
-- Schema produced by explore/generate_curves.py is "baseline.v1".

local cjson = require("cjson")
local M = {}

local SUPPORTED_SCHEMA = "baseline.v1"

-- Load baselines.json. Returns (loaded_table, n_long, n_short, err).
function M.load(path)
    local fh, oerr = io.open(path, "r")
    if not fh then return nil, 0, 0, "open: " .. tostring(oerr) end
    local raw = fh:read("*a"); fh:close()
    if not raw or raw == "" then return nil, 0, 0, "empty file" end
    local ok, decoded = pcall(cjson.decode, raw)
    if not ok then return nil, 0, 0, "decode: " .. tostring(decoded) end
    if decoded.schema ~= SUPPORTED_SCHEMA then
        return nil, 0, 0, "unsupported schema: " .. tostring(decoded.schema)
    end
    if type(decoded.bins) ~= "table" then
        return nil, 0, 0, "missing 'bins' table"
    end
    local n_long, n_short = 0, 0
    for _, v in pairs(decoded.bins) do
        if v.mode == "long" then n_long = n_long + 1
        elseif v.mode == "short" then n_short = n_short + 1 end
    end
    return decoded, n_long, n_short, nil
end

-- Convenience: pull out the per-bin entry for a given key. Returns nil
-- if missing or if loaded is nil.
function M.lookup(loaded, bin_key)
    if not loaded or not loaded.bins or not bin_key then return nil end
    return loaded.bins[bin_key]
end

-- Bool: bin is long-mode and KB3-eligible (low-noise enough to live-monitor).
function M.kb3_eligible(loaded, bin_key)
    local e = M.lookup(loaded, bin_key)
    return e ~= nil and e.mode == "long" and e.kb3_eligible == true
        and e.kb3_threshold ~= nil and e.ref ~= nil
end

return M
