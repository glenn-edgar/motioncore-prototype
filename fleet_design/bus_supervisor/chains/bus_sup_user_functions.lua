-- chains/bus_sup_user_functions.lua — ct_* user functions for the bus supervisor.
--
-- Slice 1b: the dongle lifecycle is REAL (FFI into libbus_controller.so via
-- lib/bus_core.lua + zenoh via the vendored bindings). The supervisor contract
-- and signatures are unchanged from slice 1a:
--   DONGLE_BIND  (init) — open the bus, provision, wire the event handler
--   DONGLE_SERVE (main) — NON-BLOCKING each tick: pump the bus, serve the cmd
--                         RPC (async submit, deferred reply), publish leaves;
--                         CFL_DISABLE on fault (→ DONGLE_TERM → supervisor restart)
--   DONGLE_TERM  (term) — close the controller (flock release), mark missing
--   BUS_GATE_DOWN(fin)  — supervisor gave up on a dongle → operational=false
--
-- main.lua attaches per-dongle runtime to the blackboard before the KB runs:
--   bb._ps                 shared PubSub session
--   bb._dongles[id] = { class, instance, addr, device, roster, cmd_q }
-- DONGLE_BIND opens the bus and stamps rt.bus; restart reuses the same cmd_q.

local common = require("ct_common")
local defs   = require("ct_definitions")
local cjson  = require("cjson")
local bus_core = require("bus_core")

local M = { main = {}, one_shot = {}, boolean = {} }

local function log(fmt, ...)
    io.stderr:write("BUS_SUP: " .. string.format(fmt, ...) .. "\n"); io.stderr:flush()
end
local function cfg_of(node)
    local nd = node.node_dict or {}
    return nd.column_data or nd.user_data or nd.fn_data or nd
end

-- ---- publish helpers (A3-compatible channels) -----------------------------
local function pub_catalog(ps, rt)
    ps:publish(rt.tok_cat, cjson.encode({ schema="bus_catalog/1", class=rt.class, commands=bus_core.CATALOG }))
end
local function pub_health(ps, rt)
    ps:publish(rt.tok_health, cjson.encode(rt.g_health))
end
local function pub_interlock(ps, rt)
    ps:publish(rt.tok_il, cjson.encode(rt.g_il))
end
local function pub_reconcile(ps, rt, status)
    local ci = rt.class .. "/" .. rt.instance
    ps:publish(rt.tok_reconcile, cjson.encode({
        schema="bus_reconcile/1",
        dongles = { [rt.dongle_id] = { status=status, slaves = { [ci] = { status=status, present=(status=="PRESENT") } } } },
    }))
end
local function pub_operational(ps, tok, op, reason)
    ps:publish(tok, cjson.encode({ schema="bus_operational/1", operational=op, reason=reason }))
end

-- ---------------------------------------------------------------------------
-- Dongle lifecycle (REAL)
-- ---------------------------------------------------------------------------

M.one_shot.DONGLE_BIND = function(handle, node)
    local node_id = node.label_dict.ltree_name
    local cfg     = cfg_of(node)
    local ns      = common.alloc_node_state(handle, node_id, {})
    ns.dongle_id  = cfg.dongle_id or "?"
    ns.fail_after = tonumber(cfg.fail_after) or 0
    ns.ticks      = 0
    ns.force_fault = false
    ns.bind_failed = false

    local bb = handle.blackboard
    bb._bind_counts = bb._bind_counts or {}
    bb._bind_counts[ns.dongle_id] = (bb._bind_counts[ns.dongle_id] or 0) + 1
    local rt = (bb._dongles or {})[ns.dongle_id]
    ns.rt = rt
    if not rt then
        log("BIND   dongle %-14s — no runtime registered (main.lua) → fail", ns.dongle_id)
        ns.bind_failed = true; return
    end

    log("BIND   dongle %-14s — open %s + provision [attempt #%d]",
        ns.dongle_id, tostring(rt.device), bb._bind_counts[ns.dongle_id])

    local bus, e = bus_core.Bus.open(rt.device, rt.roster)
    if not bus then ns.bind_failed = true; log("BIND   dongle %s — open failed: %s", ns.dongle_id, tostring(e)); return end
    local ok, perr = bus:wait_ready(8000)
    if not ok then bus:close(); ns.bind_failed = true; log("BIND   dongle %s — provision failed: %s", ns.dongle_id, tostring(perr)); return end

    -- verify topology (slice 1b: single slave bound by provisioning; zombie scan
    -- = the C-core sweep already classified responders. chip_uid match → A4.)
    rt.bus = bus; ns.bus = bus
    rt.g_health = { schema="bus_health/1", class=rt.class, instance=rt.instance, addr=rt.addr, state="present" }
    rt.g_il     = { schema="bus_interlock/1", class=rt.class, instance=rt.instance, tripped=false }

    -- bus events → health / interlock leaves (A3 mapping)
    bus:set_event_handler(function(kind, addr, status, aux, data)
        if kind == 4 then
            rt.g_health.state = (status == 1) and "present" or "missing"; pub_health(bb._ps, rt)
        elseif kind == 2 then
            rt.g_il.tripped = (aux % 2) == 1; pub_interlock(bb._ps, rt)
        elseif kind == 3 then
            if data and #data >= 6 then rt.g_il.tf = data:byte(6) end; pub_interlock(bb._ps, rt)
        end
    end)

    pub_health(bb._ps, rt); pub_reconcile(bb._ps, rt, "PRESENT")
    log("VERIFY dongle %-14s — provisioned, slave present → SERVING", ns.dongle_id)
end

M.main.DONGLE_SERVE = function(handle, bool_fn, node, event_id, event_data)
    if event_id ~= defs.CFL_TIMER_EVENT then return defs.CFL_CONTINUE end
    local node_id = node.label_dict.ltree_name
    local ns = common.get_node_state(handle, node_id)
    if not ns then return defs.CFL_CONTINUE end
    if ns.bind_failed then
        log("FAULT  dongle %-14s — bind failed → CFL_DISABLE (supervisor restarts)", ns.dongle_id)
        return defs.CFL_DISABLE
    end
    local bb, rt, bus = handle.blackboard, ns.rt, ns.bus
    if not bus then return defs.CFL_CONTINUE end

    -- 1) pump the bus (drain async completions → fire deferred replies + events)
    bus:poll()

    -- 2) serve the cmd RPC, non-blocking: submit_async, reply on completion
    local req = rt.cmd_q:poll()
    while req do
        local r = req
        local ok, j = pcall(cjson.decode, r:payload())
        if ok and type(j) == "table" and j.command then
            if j.command == "__fault" then          -- test hook: controlled fault
                ns.force_fault = true
                r:reply(cjson.encode({ ok=true, result={ faulting=true } }))
            else
                local fn = j.admin and bus.submit_admin or bus.submit_async
                fn(bus, rt.addr, j.command, j.args, j.timeout_ms or 1000, function(err, res)
                    r:reply(cjson.encode({ ok = (err == nil), result = res, error = err }))
                end)
            end
        else
            r:reply(cjson.encode({ ok=false, error="bad request" }))
        end
        req = rt.cmd_q:poll()
    end

    -- 3) periodic leaves (driven off handle.timestamp seconds)
    local now = handle.timestamp or 0
    if now - (rt._t_cat or -100) >= 5 then pub_catalog(bb._ps, rt); rt._t_cat = now end
    if now - (rt._t_hb  or -100) >= 3 then
        pub_health(bb._ps, rt); pub_reconcile(bb._ps, rt, "PRESENT")
        pub_operational(bb._ps, bb._tok_oper, true, "ok"); rt._t_hb = now
    end

    -- 4) fault?
    ns.ticks = ns.ticks + 1
    if ns.force_fault or (ns.fail_after > 0 and ns.ticks >= ns.fail_after) then
        log("FAULT  dongle %-14s — %s → CFL_DISABLE (supervisor restarts)",
            ns.dongle_id, ns.force_fault and "injected via __fault" or ("auto after "..ns.ticks.." ticks"))
        return defs.CFL_DISABLE
    end
    return defs.CFL_CONTINUE
end

M.one_shot.DONGLE_TERM = function(handle, node)
    local node_id = node.label_dict.ltree_name
    local ns = common.get_node_state(handle, node_id) or {}
    local bb, rt = handle.blackboard, ns.rt
    if ns.bus then ns.bus:close(); ns.bus = nil; if rt then rt.bus = nil end end
    if rt and bb._ps then
        rt.g_health = rt.g_health or { schema="bus_health/1", class=rt.class, instance=rt.instance, addr=rt.addr }
        rt.g_health.state = "missing"
        pub_health(bb._ps, rt); pub_reconcile(bb._ps, rt, "MISSING")
    end
    log("TERM   dongle %-14s — closed controller, flock released (clean)", ns.dongle_id or "?")
end

M.one_shot.BUS_GATE_DOWN = function(handle, node)
    log("GATE DOWN — a dongle exceeded the restart limit; system NOT operational")
    handle.blackboard.bus_operational = false
    local bb = handle.blackboard
    if bb._ps then pub_operational(bb._ps, bb._tok_oper, false, "dongle restart limit exceeded") end
end

M.registry = { main = M.main, one_shot = M.one_shot, boolean = M.boolean }
return M
