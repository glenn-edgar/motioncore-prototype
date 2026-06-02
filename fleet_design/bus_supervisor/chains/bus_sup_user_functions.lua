-- chains/bus_sup_user_functions.lua — ct_* user functions for the bus supervisor KB.
--
-- ct_* (dict runtime) signatures:
--   one_shot : function(handle, node)                                 -- init / term / finalize
--   main     : function(handle, bool_fn, node, event_id, event_data)  -- ticked; returns a return code
-- Per-node config travels in node.node_dict; per-node state via ct_common.
--
-- SLICE 1 (this file) stubs the dongle lifecycle so the SUPERVISION model can be
-- proven on the dev host with no bus hardware:
--   DONGLE_BIND  — pretend to claim the BC (logs the attempt; counts on bb)
--   DONGLE_SERVE — pretend to serve; injects a fault after `fail_after` ticks
--   DONGLE_TERM  — pretend to release flock/controller (the cleanup guarantee)
--   BUS_GATE_DOWN— supervisor finalize: a dongle exceeded the restart limit
-- Slice 1b replaces BIND/SERVE/TERM with real FFI into libbus_controller.so +
-- zenoh; the signatures and the supervisor contract stay identical.

local common = require("ct_common")
local defs   = require("ct_definitions")

local M = { main = {}, one_shot = {}, boolean = {} }

local function log(fmt, ...)
    io.stderr:write("BUS_SUP: " .. string.format(fmt, ...) .. "\n")
    io.stderr:flush()
end

-- The `data` passed to define_column lands in node_dict; be tolerant of where
-- the builder nests it (column_data / user_data / fn_data / node_dict itself).
local function cfg_of(node)
    local nd = node.node_dict or {}
    return nd.column_data or nd.user_data or nd.fn_data or nd
end

-- ---------------------------------------------------------------------------
-- Dongle lifecycle (STUB — slice 1)
-- ---------------------------------------------------------------------------

-- init_fn: claim the unique bus controller + verify topology. Runs on every
-- (re)enable, so each supervisor restart re-binds from scratch.
M.one_shot.DONGLE_BIND = function(handle, node)
    local node_id = node.label_dict.ltree_name
    local cfg     = cfg_of(node)
    local ns      = common.alloc_node_state(handle, node_id, {})
    ns.dongle_id  = cfg.dongle_id or "?"
    ns.fail_after = tonumber(cfg.fail_after) or 0
    ns.ticks      = 0

    local bb = handle.blackboard
    bb._bind_counts = bb._bind_counts or {}
    bb._bind_counts[ns.dongle_id] = (bb._bind_counts[ns.dongle_id] or 0) + 1

    log("BIND   dongle %-14s — probe+match chip_uid+flock+open [attempt #%d]",
        ns.dongle_id, bb._bind_counts[ns.dongle_id])
    -- slice 1b: scan /dev/ttyACM*, match chip_uid, flock, bus_open, verify
    -- topology + zombie scan here; on failure set ns.bind_failed = true.
end

-- main_fn: the steady-state serve. Returns CFL_CONTINUE normally; CFL_DISABLE on
-- fault (which disables this node → runs DONGLE_TERM → supervisor restarts it).
M.main.DONGLE_SERVE = function(handle, bool_fn, node, event_id, event_data)
    if event_id ~= defs.CFL_TIMER_EVENT then return defs.CFL_CONTINUE end

    local node_id = node.label_dict.ltree_name
    local ns = common.get_node_state(handle, node_id)
    if not ns then return defs.CFL_CONTINUE end   -- bind hasn't run yet

    ns.ticks = ns.ticks + 1
    if ns.ticks == 1 then
        log("VERIFY dongle %-14s — topology ok, zombie scan clean → SERVING",
            ns.dongle_id)
    end
    -- slice 1b: bus:poll() (async drain+issue), serve cmd RPC, periodic
    -- announce, pet the heartbeat — all non-blocking.

    if ns.fail_after > 0 and ns.ticks >= ns.fail_after then
        log("FAULT  dongle %-14s — injected after %d ticks → CFL_DISABLE (supervisor restarts)",
            ns.dongle_id, ns.ticks)
        return defs.CFL_DISABLE
    end
    return defs.CFL_CONTINUE
end

-- term_fn: release the bus claim. Runs on EVERY disable (fault, restart,
-- shutdown) — the init/term guarantee that makes restart clean.
M.one_shot.DONGLE_TERM = function(handle, node)
    local node_id = node.label_dict.ltree_name
    local ns = common.get_node_state(handle, node_id) or {}
    log("TERM   dongle %-14s — released flock + closed controller (clean)",
        ns.dongle_id or "?")
    -- slice 1b: bus_close(controller) + flock release here.
end

-- ---------------------------------------------------------------------------
-- Supervisor finalize — the bootloop escalation / system gate
-- ---------------------------------------------------------------------------

-- Called (with the SUPERVISOR node) when a child exceeds the restart limit
-- (leaky bucket: max_reset_number within reset_window). The system is no
-- longer operational.
M.one_shot.BUS_GATE_DOWN = function(handle, node)
    log("GATE DOWN — a dongle exceeded the restart limit; system NOT operational")
    handle.blackboard.bus_operational = false
    -- slice 1b: publish fleet/bus/operational {operational=false, reason=...}
end

M.registry = { main = M.main, one_shot = M.one_shot, boolean = M.boolean }
return M
