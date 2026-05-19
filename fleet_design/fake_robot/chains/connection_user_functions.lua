-- chains/connection_user_functions.lua — user fns for the connection KB.
--
-- Per option (c): the `connecting` state is fully implemented (real register
-- RPC call); other states are stubs that log and advance, so the pump can
-- be exercised end-to-end before the remaining state logic is filled in.
--
-- Caller (main.lua) attaches context to the blackboard before activating:
--   bb._identity, bb._class_spec, bb._pubsub, bb._rpc

local cjson = require("cjson")
local defs  = require("ct_definitions")

local M = {}
M.main     = {}
M.one_shot = {}
M.boolean  = {}

local function now_s() return os.time() end

-- ---------------------------------------------------------------------------
-- Lifecycle one-shots
-- ---------------------------------------------------------------------------

M.one_shot.CONNECTION_INIT = function(h, node)
    local bb = h.blackboard
    bb.state             = "connecting"
    bb.register_attempt  = 0
    bb.seq               = 0
    bb.last_heartbeat_seen = 0
    bb.controller_id     = ""
    bb.ack_ts            = 0
    bb.backoff_until     = 0
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
    if now_s() < bb.backoff_until then return end

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
        ts           = now_s(),
    })

    io.stderr:write(string.format(
        "CONNECTION_KB [%s]: register attempt #%d on fleet/admin/register (timeout=2s)\n",
        id.namespace, bb.register_attempt))

    local reply, err = rpc:call("fleet/admin/register", req, 2000)
    if reply then
        local ok2, dec = pcall(cjson.decode, reply)
        if ok2 and dec and dec.ok then
            bb.controller_id = tostring(dec.controller_id or "")
            bb.ack_ts        = tonumber(dec.ts) or now_s()
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

    bb.backoff_until = now_s() + 1
end

state_handlers["ack'd"] = function(h, bb)
    io.stderr:write(string.format(
        "CONNECTION_KB [%s]: ack'd state stub → state=namespace_up\n",
        bb._identity.namespace))
    bb.state = "namespace_up"
end

state_handlers.namespace_up = function(h, bb)
    io.stderr:write(string.format(
        "CONNECTION_KB [%s]: namespace_up state stub → state=operating\n",
        bb._identity.namespace))
    bb.state = "operating"
end

state_handlers.operating = function(h, bb)
    -- Stub: hold here. Full implementation adds heartbeat publish/watch + disconnect emission.
end

state_handlers.disconnected = function(h, bb)
    io.stderr:write(string.format(
        "CONNECTION_KB [%s]: disconnected stub → state=connecting\n",
        bb._identity.namespace))
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
