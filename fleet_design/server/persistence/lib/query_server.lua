-- server/persistence/lib/query_server.lua — RPC dispatcher for the
-- persistence query API. See QUERY_API.md for the contract.
--
-- This module is pure dispatch + envelope: zero Zenoh / FFI / file I/O.
-- Driver (main.lua) owns the RPC server, polls the queue, and calls
-- :handle(payload_str) -> reply_payload_str on every request.

local cjson = require("cjson")

local M = {}
M.__index = M

local SCHEMA_VERSION = "persistence_query/1"

-- Normalize a leaf path. Spec accepts both `cimis/station/sample` and
-- `cimis.station.sample`. Internal leaf tables are keyed by the `/` form
-- (matches what the robot announces), so normalize towards that.
local function normalize_tail(path)
    if type(path) ~= "string" or path == "" then return nil end
    return (path:gsub("%.", "/"))
end

local function ok_reply(data, next_page, stats)
    local r = { ok = true, data = data }
    if next_page then r.next_page = next_page end
    if stats     then r.stats     = stats     end
    return r
end

local function err_reply(code, msg)
    return { ok = false, error = { code = code, msg = msg or code } }
end

-- ------------------------------------------------------------------
-- Constructor
-- ------------------------------------------------------------------

function M.new(persistence, opts)
    assert(persistence, "persistence required")
    opts = opts or {}
    return setmetatable({
        p               = persistence,
        max_page_rows   = opts.max_page_rows   or 100,
        max_reply_bytes = opts.max_reply_bytes or 4096,
        log_fn          = opts.log_fn,   -- optional: function(fmt, ...)
    }, M)
end

function M:_log(fmt, ...)
    if self.log_fn then self.log_fn(fmt, ...) end
end

-- ------------------------------------------------------------------
-- Leaf lookup helper
-- ------------------------------------------------------------------

-- Find the instance state for a kb_name. p.instances is keyed by
-- "<class>/<instance>" not kb_name, so we scan. Caches as we go.
function M:_state_for_kb(kb_name)
    if self._kb_index and self._kb_index[kb_name] then
        return self._kb_index[kb_name]
    end
    self._kb_index = self._kb_index or {}
    for _, st in pairs(self.p.instances) do
        self._kb_index[st.kb_name] = st
    end
    return self._kb_index[kb_name]
end

-- Resolve (kb_name, path) -> leaf info, or (nil, err_reply).
function M:_resolve_leaf(args, expect_kind)
    if type(args) ~= "table" then
        return nil, err_reply("bad_request", "args must be an object")
    end
    local kb_name = args.kb_name
    local path    = args.path
    if type(kb_name) ~= "string" or kb_name == "" then
        return nil, err_reply("bad_request", "kb_name required")
    end
    local tail = normalize_tail(path)
    if not tail then
        return nil, err_reply("bad_request", "path required")
    end
    local st = self:_state_for_kb(kb_name)
    if not st then
        return nil, err_reply("not_found", "unknown kb_name: " .. kb_name)
    end
    local leaf = st.leaves and st.leaves[tail]
    if not leaf then
        return nil, err_reply("not_found",
            string.format("path '%s' not declared on kb '%s'", tail, kb_name))
    end
    if expect_kind and leaf.kind ~= expect_kind then
        return nil, err_reply("bad_request",
            string.format("path '%s' is kind '%s', expected '%s'",
                tail, leaf.kind, expect_kind))
    end
    return leaf
end

-- ------------------------------------------------------------------
-- Op handlers — v1 surface
-- ------------------------------------------------------------------

-- latest(kb_name, path) → status row or null.
function M:op_latest(args)
    local leaf, errr = self:_resolve_leaf(args, "status")
    if not leaf then return errr end
    local ok, data = pcall(function()
        return self.p.rt:get_status_data(leaf.ltree_path)
    end)
    if not ok then
        self:_log("op_latest %s: %s", leaf.ltree_path, tostring(data))
        return err_reply("internal", "get_status_data failed")
    end
    -- `data` may be nil (no value yet) or a structured object.
    return ok_reply(data == nil and cjson.null or data)
end

-- list_kbs() → list of kbs persistence currently knows about.
function M:op_list_kbs(_args)
    local out = {}
    for _, st in pairs(self.p.instances) do
        local n = 0
        if st.leaves then for _ in pairs(st.leaves) do n = n + 1 end end
        out[#out + 1] = {
            kb_name    = st.kb_name,
            class      = st.class,
            instance   = st.instance,
            leaf_count = n,
        }
    end
    table.sort(out, function(a, b) return a.kb_name < b.kb_name end)
    return ok_reply(out)
end

-- v1 deferred ops — declared so the dispatcher returns a consistent
-- error rather than `unsupported_op` (lets clients probe).
function M:op_stream(_args)
    return err_reply("unsupported_op", "stream() lands in slice-2")
end
function M:op_latest_stream(_args)
    return err_reply("unsupported_op", "latest_stream() lands in slice-2")
end
function M:op_list_leaves(_args)
    return err_reply("unsupported_op", "list_leaves() lands in slice-2")
end

-- ------------------------------------------------------------------
-- Dispatcher + envelope
-- ------------------------------------------------------------------

local DISPATCH = {
    latest         = "op_latest",
    list_kbs       = "op_list_kbs",
    stream         = "op_stream",
    latest_stream  = "op_latest_stream",
    list_leaves    = "op_list_leaves",
}

function M:handle(req_payload)
    -- Decode
    if type(req_payload) ~= "string" or req_payload == "" then
        return cjson.encode(err_reply("bad_request", "empty request"))
    end
    local ok, req = pcall(cjson.decode, req_payload)
    if not ok or type(req) ~= "table" then
        return cjson.encode(err_reply("bad_request", "invalid JSON"))
    end
    -- Dispatch
    local op = req.op
    if type(op) ~= "string" then
        return cjson.encode(err_reply("bad_request", "op required"))
    end
    local fn_name = DISPATCH[op]
    if not fn_name then
        return cjson.encode(err_reply("unsupported_op", "unknown op: " .. op))
    end
    local handler_ok, reply = pcall(self[fn_name], self, req.args or {})
    if not handler_ok then
        self:_log("dispatch %s raised: %s", op, tostring(reply))
        return cjson.encode(err_reply("internal", "handler raised"))
    end
    -- Encode + size-cap
    local enc_ok, encoded = pcall(cjson.encode, reply)
    if not enc_ok then
        return cjson.encode(err_reply("internal", "encode failed: " .. tostring(encoded)))
    end
    if #encoded > self.max_reply_bytes then
        return cjson.encode(err_reply("payload_too_big",
            string.format("reply %d > max_reply_bytes %d",
                #encoded, self.max_reply_bytes)))
    end
    return encoded
end

-- ------------------------------------------------------------------
-- Service announce builder — driver encodes + publishes.
-- ------------------------------------------------------------------

-- opts = { service_id, rpc_token_key, republish_s }
function M:announce_payload(opts)
    assert(opts and opts.service_id     , "service_id required")
    assert(opts.rpc_token_key           , "rpc_token_key required")
    assert(opts.republish_s             , "republish_s required")
    local kbs = {}
    for _, st in pairs(self.p.instances) do
        kbs[#kbs + 1] = {
            kb_name  = st.kb_name,
            class    = st.class,
            instance = st.instance,
        }
    end
    table.sort(kbs, function(a, b) return a.kb_name < b.kb_name end)
    return {
        schema          = "persistence_service/1",
        service_id      = opts.service_id,
        rpc_token_key   = opts.rpc_token_key,
        republish_s     = opts.republish_s,
        max_page_rows   = self.max_page_rows,
        max_reply_bytes = self.max_reply_bytes,
        query_schema    = SCHEMA_VERSION,
        kbs             = kbs,
    }
end

return M
