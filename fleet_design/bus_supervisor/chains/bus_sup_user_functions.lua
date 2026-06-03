-- chains/bus_sup_user_functions.lua — ct_* user functions for the bus supervisor.
--
-- Slice 2 (option B): the dongle lifecycle is a NON-BLOCKING phase machine kept
-- inside DONGLE_SERVE (the per-tick main-fn). DONGLE_BIND shrinks to allocate-
-- only so nothing blocks the single thread; every phase yields each tick so N
-- dongles + the RPC drain all keep running while one is provisioning.
--
--   DONGLE_BIND  (init) — allocate node state, set phase="wait_turn". NO bus I/O.
--   DONGLE_SERVE (main) — phase machine, one bounded step per tick:
--       wait_turn   gate: spin until brought_up >= k-1 (serial cold start); then
--                   open the bus (non-blocking) and enter provisioning.
--       provisioning multi-tick: provision_step() each tick until ready / failed
--                   / deadline. NEVER blocks (this was the bug — wait_ready(8000)).
--       verify      wire health/interlock + event handler, publish PRESENT, and
--                   bump the gate brought_up=max(.,k) (opens k+1), enter serving.
--       serving     steady-state: pump bus, serve cmd RPC async, publish leaves;
--                   CFL_DISABLE on fault (→ DONGLE_TERM → supervisor restart).
--       fault       CFL_DISABLE.
--   DONGLE_TERM  (term) — close the controller (flock release) in ANY phase, mark
--                   missing, clear dongle_serving. Always runs on disable.
--   BUS_GATE_DOWN(fin)  — supervisor gave up on a dongle → operational=false.
--
-- The GATE (serial cold-start ordering for N dongles, restart-safe):
--   blackboard.brought_up   monotonic high-water mark — "dongles 1..N have
--                           reached SERVING at least once". Only ever increases,
--                           so a restarted child re-checks it, finds its index
--                           already cleared, and rebinds immediately (no re-
--                           serialization). It is shared blackboard state, NOT a
--                           consumable event, so restart-replay can't deadlock.
--
-- main.lua attaches per-dongle runtime to the blackboard before the KB runs:
--   bb._ps                 shared PubSub session
--   bb._dongles[id] = { class, instance, addr, device, roster, cmd_q, tok_* }
--   bb.dongle_serving      set { [id]=true } — main aggregates it for operational
--   bb.brought_up          gate high-water mark (init 0)
-- Restart reuses the same rt (and its cmd_q); ns (phase, bus) is per-incarnation.

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
-- Dongle lifecycle — NON-BLOCKING phase machine (option B)
-- ---------------------------------------------------------------------------

local PROVISION_TIMEOUT_S = 10   -- generous: a cold BC resync can take seconds

-- BIND: allocate-only. NO bus I/O — the open happens in the wait_turn phase once
-- the gate clears, so a blocked/slow open can never stall the single thread.
M.one_shot.DONGLE_BIND = function(handle, node)
    local node_id = node.label_dict.ltree_name
    local cfg     = cfg_of(node)
    local ns      = common.alloc_node_state(handle, node_id, {})
    ns.dongle_id   = cfg.dongle_id or "?"
    ns.k           = tonumber(cfg.bring_up_index) or 1   -- serial cold-start order
    ns.fail_after  = tonumber(cfg.fail_after) or 0
    ns.ticks       = 0
    ns.force_fault = false
    ns.phase       = "wait_turn"

    local bb = handle.blackboard
    bb.brought_up   = bb.brought_up or 0
    bb._bind_counts = bb._bind_counts or {}
    bb._bind_counts[ns.dongle_id] = (bb._bind_counts[ns.dongle_id] or 0) + 1
    local attempt = bb._bind_counts[ns.dongle_id]
    ns.rt = (bb._dongles or {})[ns.dongle_id]
    if not ns.rt then
        log("BIND   dongle %-14s — no runtime registered (main.lua) → fault", ns.dongle_id)
        ns.phase = "fault"; return
    end
    -- Restart backoff: the one_for_one supervisor re-enables a failed child on
    -- the SAME tick it disables, so with non-blocking fast-fail provisioning the
    -- retries would otherwise fire microseconds apart — tripping the leaky bucket
    -- before the BC can resync (the known "attempt #1 fails, #2 succeeds after a
    -- SIGKILL" bench case). Space attempts so a transient recovers on retry while
    -- a genuinely dead device still gates down within the reset window.
    ns.open_after = (handle.timestamp or 0) + math.min((attempt - 1) * 3, 6)
    log("BIND   dongle %-14s (k=%d) — allocated, waiting for turn [attempt #%d]",
        ns.dongle_id, ns.k, attempt)
end

-- enter serving: wire the steady-state leaves + event handler, open the gate for
-- the next dongle. Called once on the verify→serving transition.
local function enter_serving(handle, ns)
    local bb, rt, bus = handle.blackboard, ns.rt, ns.bus
    rt.g_health = { schema="bus_health/1", class=rt.class, instance=rt.instance, addr=rt.addr, state="present" }
    rt.g_il     = { schema="bus_interlock/1", class=rt.class, instance=rt.instance, tripped=false }
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
    bb.dongle_serving = bb.dongle_serving or {}
    bb.dongle_serving[ns.dongle_id] = true
    -- open the gate for k+1 (monotonic — restart never lowers it)
    bb.brought_up = math.max(bb.brought_up or 0, ns.k)
    log("SERVE  dongle %-14s — verified, slave present → SERVING (gate=%d)",
        ns.dongle_id, bb.brought_up)
end

M.main.DONGLE_SERVE = function(handle, bool_fn, node, event_id, event_data)
    if event_id ~= defs.CFL_TIMER_EVENT then return defs.CFL_CONTINUE end
    local node_id = node.label_dict.ltree_name
    local ns = common.get_node_state(handle, node_id)
    if not ns then return defs.CFL_CONTINUE end
    local bb, rt = handle.blackboard, ns.rt
    local now = handle.timestamp or 0

    -- ---- gate: spin (stay enabled = healthy to the supervisor) until turn ----
    if ns.phase == "wait_turn" then
        if (bb.brought_up or 0) < (ns.k - 1) then return defs.CFL_CONTINUE end
        if now < (ns.open_after or 0) then return defs.CFL_CONTINUE end  -- restart backoff
        local bus, e = bus_core.Bus.open(rt.device, rt.roster)   -- non-blocking
        if not bus then
            log("FAULT  dongle %-14s — open %s failed: %s", ns.dongle_id, tostring(rt.device), tostring(e))
            ns.phase = "fault"; return defs.CFL_CONTINUE
        end
        ns.bus = bus; rt.bus = bus
        ns.prov_deadline = now + PROVISION_TIMEOUT_S
        ns.phase = "provisioning"
        log("BIND   dongle %-14s — opened %s, provisioning…", ns.dongle_id, tostring(rt.device))
        return defs.CFL_CONTINUE
    end

    -- ---- provisioning: one non-blocking step per tick ----
    if ns.phase == "provisioning" then
        local st = ns.bus:provision_step()
        if st == "ready" then
            ns.phase = "verify"
        elseif st == "failed" or now > (ns.prov_deadline or 0) then
            log("FAULT  dongle %-14s — provision %s → CFL_DISABLE (supervisor restarts)",
                ns.dongle_id, st == "failed" and "rejected" or "timeout")
            ns.phase = "fault"
        end
        return defs.CFL_CONTINUE
    end

    -- ---- verify: one-shot transition into steady state ----
    if ns.phase == "verify" then
        enter_serving(handle, ns)   -- topology verified by provisioning sweep; A4 adds chip_uid
        ns.phase = "serving"
        return defs.CFL_CONTINUE
    end

    -- ---- fault: disable → DONGLE_TERM → supervisor restart ----
    if ns.phase == "fault" then
        return defs.CFL_DISABLE
    end

    -- ---- serving: steady state ----
    local bus = ns.bus
    if not bus then ns.phase = "fault"; return defs.CFL_CONTINUE end

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

    -- 3) periodic leaves (driven off handle.timestamp seconds). Operational is
    -- published by main.lua at the supervisor level (all configured dongles
    -- SERVING), NOT here — a single dongle no longer asserts the whole gate.
    if now - (rt._t_cat or -100) >= 5 then pub_catalog(bb._ps, rt); rt._t_cat = now end
    if now - (rt._t_hb  or -100) >= 3 then
        pub_health(bb._ps, rt); pub_reconcile(bb._ps, rt, "PRESENT"); rt._t_hb = now
    end

    -- 4) fault?
    ns.ticks = ns.ticks + 1
    if ns.force_fault or (ns.fail_after > 0 and ns.ticks >= ns.fail_after) then
        log("FAULT  dongle %-14s — %s → CFL_DISABLE (supervisor restarts)",
            ns.dongle_id, ns.force_fault and "injected via __fault" or ("auto after "..ns.ticks.." ticks"))
        if bb.dongle_serving then bb.dongle_serving[ns.dongle_id] = nil end
        return defs.CFL_DISABLE
    end
    return defs.CFL_CONTINUE
end

M.one_shot.DONGLE_TERM = function(handle, node)
    local node_id = node.label_dict.ltree_name
    local ns = common.get_node_state(handle, node_id) or {}
    local bb, rt = handle.blackboard, ns.rt
    if bb.dongle_serving and ns.dongle_id then bb.dongle_serving[ns.dongle_id] = nil end
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
