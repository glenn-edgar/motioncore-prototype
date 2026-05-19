-- chains/connection_user_functions.lua — user fns for the connection KB.
--
-- Full state machine per decisions #30, #31, #32:
--   connecting    register RPC on fleet/admin/register (sync, 2 s timeout, backoff retry)
--   ack'd         subscribe fleet/admin/heartbeat + <ns>/desired_state
--   namespace_up  publish capabilities + hardware + state=ready, run class hook,
--                 prime heartbeat-watch timestamp
--   operating     1 Hz heartbeat publish on <ns>/heartbeat; drain pubsub queue;
--                 update last_heartbeat_seen on each fleet/admin/heartbeat;
--                 if > disconnect_threshold_s with no heartbeat → disconnected
--   disconnected  drop subs, close+reopen sessions, reset counters → connecting
--
-- Caller (main.lua) attaches context to the blackboard before activating:
--   bb._identity, bb._class_spec, bb._pubsub, bb._rpc

local cjson = require("cjson")
local defs  = require("ct_definitions")
local clock = require("clock")

local now_ms = clock.now_ms

-- Wall-clock epoch seconds (fractional double) for ts fields on the wire.
local function wall_s() return clock.wall_now().epoch_s end

local M = {}
M.main     = {}
M.one_shot = {}
M.boolean  = {}

-- ---------------------------------------------------------------------------
-- Lifecycle one-shots
-- ---------------------------------------------------------------------------

M.one_shot.CONNECTION_INIT = function(h, node)
    local bb = h.blackboard
    bb.state                  = "connecting"
    bb.register_attempt       = 0
    bb.seq                    = 0
    bb.last_heartbeat_seen_ms = 0
    bb.controller_id          = ""
    bb.ack_ts                 = 0
    bb.backoff_until_ms       = 0
    io.stderr:write(string.format(
        "CONNECTION_KB [%s]: init → state=connecting\n",
        bb._identity.namespace))
end

M.one_shot.CONNECTION_TERM = function(h, node)
    io.stderr:write("CONNECTION_KB: term\n")
end

-- ---------------------------------------------------------------------------
-- Per-state handlers (dispatched by CONNECTION_MAIN)
-- ---------------------------------------------------------------------------

local state_handlers = {}

state_handlers.connecting = function(h, bb)
    if now_ms() < bb.backoff_until_ms then return end

    bb.register_attempt = bb.register_attempt + 1
    local id  = bb._identity
    local cs  = bb._class_spec
    local rpc = bb._rpc

    local req = cjson.encode({
        class        = id.class,
        instance     = id.instance,
        chip_uid     = id.chip_uid,
        fw_version   = id.fw_version,
        capabilities = cs.capabilities,
        ts           = wall_s(),
    })

    io.stderr:write(string.format(
        "CONNECTION_KB [%s]: register attempt #%d on fleet/admin/register (timeout=2s)\n",
        id.namespace, bb.register_attempt))

    local reply, err = rpc:call("fleet/admin/register", req, 2000)
    if reply then
        local ok2, dec = pcall(cjson.decode, reply)
        if ok2 and dec and dec.ok then
            bb.controller_id = tostring(dec.controller_id or "")
            bb.ack_ts        = tonumber(dec.ts) or wall_s()
            bb.state         = "ack'd"
            io.stderr:write(string.format(
                "CONNECTION_KB [%s]: registered (controller_id=%s, ts=%d) → state=ack'd\n",
                id.namespace, bb.controller_id, bb.ack_ts))
            return
        end
        io.stderr:write(string.format(
            "CONNECTION_KB [%s]: malformed register reply (%s), retry in 1s\n",
            id.namespace, tostring(reply)))
    else
        io.stderr:write(string.format(
            "CONNECTION_KB [%s]: register failed: %s — retry in 1s\n",
            id.namespace, tostring(err)))
    end

    bb.backoff_until_ms = now_ms() + 1000
end

state_handlers["ack'd"] = function(h, bb)
    local id = bb._identity
    local ps = bb._pubsub
    bb._sub_heartbeat     = ps:subscribe("fleet/admin/heartbeat",
                                          { kind = "ctrl_heartbeat" })
    bb._sub_desired_state = ps:subscribe(id.namespace .. "/desired_state",
                                          { kind = "desired_state" })
    io.stderr:write(string.format(
        "CONNECTION_KB [%s]: subscribed (fleet/admin/heartbeat, %s/desired_state) → state=namespace_up\n",
        id.namespace, id.namespace))
    bb.state = "namespace_up"
end

state_handlers.namespace_up = function(h, bb)
    local id = bb._identity
    local cs = bb._class_spec
    local ps = bb._pubsub

    ps:publish(id.namespace .. "/capabilities", cjson.encode(cs.capabilities))
    ps:publish(id.namespace .. "/hardware", cjson.encode({
        chip_uid   = id.chip_uid,
        fw_version = id.fw_version,
        first_seen = id.first_seen,
    }))
    ps:publish(id.namespace .. "/state", cjson.encode({
        state = "ready",
        ts    = wall_s(),
    }))

    local ok, err = pcall(cs.on_namespace_up, ps, id, bb)
    if not ok then
        io.stderr:write(string.format(
            "CONNECTION_KB [%s]: on_namespace_up hook errored: %s\n",
            id.namespace, tostring(err)))
    end

    -- Prime the heartbeat-watch clock at register-success time
    bb.last_heartbeat_seen_ms = now_ms()
    bb._last_hb_pub_ms = 0

    io.stderr:write(string.format(
        "CONNECTION_KB [%s]: published core leaves + ran class hook → state=operating\n",
        id.namespace))
    bb.state = "operating"
end

state_handlers.operating = function(h, bb)
    local id      = bb._identity
    local ps      = bb._pubsub
    local t_now   = now_ms()

    -- 1 Hz heartbeat publish on <namespace>/heartbeat
    if t_now - (bb._last_hb_pub_ms or 0) >= 1000 then
        bb._last_hb_pub_ms = t_now
        bb.seq = bb.seq + 1
        ps:publish(id.namespace .. "/heartbeat", cjson.encode({
            seq = bb.seq, ts = wall_s(),
        }))
    end

    -- Drain incoming pub/sub queues
    local msgs = ps:poll_all()
    for _, m in ipairs(msgs) do
        if m.user and m.user.kind == "ctrl_heartbeat" then
            bb.last_heartbeat_seen_ms = now_ms()
        elseif m.user and m.user.kind == "desired_state" then
            io.stderr:write(string.format(
                "CONNECTION_KB [%s]: desired_state recv: %s\n",
                id.namespace, m.payload))
        else
            io.stderr:write(string.format(
                "CONNECTION_KB [%s]: unexpected msg on %s: %s\n",
                id.namespace, m.topic, m.payload))
        end
    end

    -- Disconnect-watch
    local since_ms = now_ms() - bb.last_heartbeat_seen_ms
    if since_ms > bb.disconnect_threshold_ms then
        io.stderr:write(string.format(
            "CONNECTION_KB [%s]: no controller heartbeat for %dms → state=disconnected\n",
            id.namespace, since_ms))
        bb.state = "disconnected"
    end
end

state_handlers.disconnected = function(h, bb)
    local id  = bb._identity
    local ps  = bb._pubsub
    local rpc = bb._rpc

    if bb._sub_heartbeat then
        pcall(function() ps:unsubscribe(bb._sub_heartbeat) end)
        bb._sub_heartbeat = nil
    end
    if bb._sub_desired_state then
        pcall(function() ps:unsubscribe(bb._sub_desired_state) end)
        bb._sub_desired_state = nil
    end

    pcall(function() ps:close() end)
    pcall(function() rpc:close() end)
    ps:open()
    rpc:open()

    bb.register_attempt = 0
    bb.backoff_until_ms = 0
    io.stderr:write(string.format(
        "CONNECTION_KB [%s]: sessions reopened → state=connecting\n",
        id.namespace))
    bb.state = "connecting"
end

-- ---------------------------------------------------------------------------
-- Main fn: per-tick dispatch
-- ---------------------------------------------------------------------------

M.main.CONNECTION_MAIN = function(h, bool_fn, node, event_id, event_data)
    if event_id ~= defs.CFL_TIMER_EVENT then return defs.CFL_CONTINUE end
    local bb = h.blackboard
    if bb.shutdown_requested then return defs.CFL_DISABLE end

    local handler = state_handlers[bb.state]
    if handler then
        handler(h, bb)
    else
        io.stderr:write("CONNECTION_KB: unknown state '" .. tostring(bb.state) .. "'\n")
    end
    return defs.CFL_CONTINUE
end

M.registry = {
    main     = M.main,
    one_shot = M.one_shot,
    boolean  = M.boolean,
}

return M
