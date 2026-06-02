-- reconcile.lua — the dongle-layer expected-vs-found reconciliation engine.
--
-- This is the supervisor's core: compare the EXPECTED inventory (the roster /
-- per-dongle config) against what is actually FOUND announcing on the bus, and
-- decide whether the system may be OPERATIONAL. It is the fractal engine from the
-- locked design ([[bus-fleet-integration-2026-06-02]]): the same shape applies
-- system->dongles here, and dongle->slaves one level down.
--
-- States (locked 4-state model): PRESENT / MISSING / UNEXPECTED / MISMATCH.
--   PRESENT    — expected and found (fresh).
--   MISSING    — expected, not found (or stale, or found-but-link-down).
--   UNEXPECTED — found, not expected (informational; does NOT drop the gate).
--   MISMATCH   — found but identity differs from expected (A4: chip-uid/class
--                verify; for A3 we have no identity to compare, so it never fires).
--
-- The system gate (operational) is fail-safe: ANY expected dongle MISSING, any
-- expected slave MISSING, or any MISMATCH drops it. UNEXPECTED is a warning only.

local M = {}

-- expected: { [dongle_id] = { slaves = { ["class/instance"] = true, ... } } }
-- found:    { [dongle_id] = { last_seen = ms, slaves = {
--                                ["class/instance"] = { present=bool, last_seen=ms, addr=n } } } }
-- now, stale_ms in the same clock as last_seen.
--
-- returns {
--   operational = bool, reason = str,
--   dongles = { [id] = { status = STATE, slaves = { [ci] = { status=STATE, present=bool } } } },
-- }
function M.reconcile(expected, found, now, stale_ms)
  local out = { operational = true, reason = "ok", dongles = {} }
  local function fault(r) if out.operational then out.operational = false; out.reason = r end end

  -- ---- expected dongles ----------------------------------------------------
  for did, exp in pairs(expected) do
    local f = found[did]
    local fresh = f and (now - (f.last_seen or 0) <= stale_ms)
    local d = { status = "PRESENT", slaves = {} }

    if not fresh then
      d.status = "MISSING"
      fault("dongle " .. did .. " MISSING")
    else
      -- expected slaves on this dongle
      for ci in pairs(exp.slaves or {}) do
        local fs = f.slaves and f.slaves[ci]
        local sfresh = fs and (now - (fs.last_seen or f.last_seen) <= stale_ms)
        if not sfresh then
          d.slaves[ci] = { status = "MISSING", present = false }
          fault("slave " .. ci .. " MISSING")
        elseif fs.present == false then
          d.slaves[ci] = { status = "MISSING", present = false }   -- found, link down
          fault("slave " .. ci .. " link down")
        else
          d.slaves[ci] = { status = "PRESENT", present = true }
        end
      end
      -- slaves the dongle announces that we did not expect
      if f.slaves then
        for ci, fs in pairs(f.slaves) do
          if not (exp.slaves and exp.slaves[ci]) then
            d.slaves[ci] = { status = "UNEXPECTED", present = fs.present == true }
          end
        end
      end
    end
    out.dongles[did] = d
  end

  -- ---- dongles found but not expected (informational) ----------------------
  for did, f in pairs(found) do
    if not expected[did] then
      local fresh = (now - (f.last_seen or 0) <= stale_ms)
      out.dongles[did] = { status = fresh and "UNEXPECTED" or "MISSING", slaves = {} }
      -- UNEXPECTED does not drop the gate (a stray dongle is a warning, not a stop).
    end
  end

  return out
end

-- Normalize an expected-inventory table whose `slaves` may be a JSON array
-- (["class/instance", ...]) into the set form ({["class/instance"]=true}).
function M.normalize_expected(t)
  local out = {}
  for did, d in pairs(t or {}) do
    local slaves = {}
    local s = d and d.slaves
    if type(s) == "table" then
      -- array form?  (#s > 0 and integer keys)
      if #s > 0 then
        for _, ci in ipairs(s) do slaves[ci] = true end
      else
        for ci, v in pairs(s) do if v then slaves[ci] = true end end
      end
    end
    out[did] = { slaves = slaves }
  end
  return out
end

return M
