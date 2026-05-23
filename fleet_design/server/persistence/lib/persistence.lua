-- server/persistence/lib/persistence.lua — the persistence service core.
--
-- Maintains one SQLite database (persistence.db) holding aux tables (status,
-- stream, ...) built by `construct_kb`. Each robot instance gets its own
-- knowledge_base (kb_name = "<class>_<instance>") — a query-time partition.
-- Inside that kb, paths mirror the Zenoh keys verbatim: Zenoh
-- `<class>/<instance>/<tail>` becomes ltree
-- `<kb_name>.<kind>.<tail-with-/-as-.>` where <kind> is the row-type label
-- ("stream" / "status") that construct_kb inserts between header and field.
--
-- The persistence service is a pure subscriber: it does NOT know the
-- topology a priori. Robots announce on `fleet/admin/persistence_topology_announce`
-- (decision: robot-announces, persistence-builds-on-first-contact). On each
-- announce, apply_topology() reconciles by diffing the announced entry set
-- against the cached in-memory leaves; ADDED leaves get declared + pre-
-- allocated, REMOVED leaves get returned to the driver so it can close their
-- subs. Subsequent samples are dispatched via dispatch() to
-- push_stream_data / set_status_data.
--
-- Schema policy is ADDITIVE-ONLY: removed leaves' rows stay in the DB
-- (both stream history and last-known status are preserved). The cost of
-- a stale status row is small (a dashboard timestamp filter handles it);
-- the cost of accidentally trimming live data on a malformed announce is
-- much larger. Explicit decommissioning is a future operator tool, not an
-- automatic side effect of a topology change.
--
-- Construct semantics: `construct_kb.add_node` always INSERTs, so we cannot
-- naïvely re-declare a kb. The reconciliation only invokes the construct
-- path for the ADDED set (so each ADDED leaf is declared exactly once), and
-- gates on a content-hash to short-circuit identical re-announces (the
-- common case after a robot reboot).

local cjson = require("cjson")
local CDT   = require("construct_data_tables")
local KBDS  = require("kb_data_structures")

local DEFAULT_DB_TABLE = "knowledge_base"

local M = {}
M.__index = M

local function file_exists(path)
    local f = io.open(path, "rb")
    if f then f:close(); return true end
    return false
end

-- Convert a Zenoh tail ("cimis/station/sample") to an ltree field name
-- ("cimis.station.sample"). `/` is the only special character we expect.
local function tail_to_field(tail)
    return (tail:gsub("/", "."))
end

-- Count non-nil keys of a map. Lua's `#t` only works on integer-indexed
-- sequences; this is for sparse / string-keyed tables.
local function count_keys(t)
    local n = 0
    for _ in pairs(t) do n = n + 1 end
    return n
end

function M.new(db_path)
    assert(db_path, "db_path required")
    local self = setmetatable({}, M)
    self.db_path  = db_path
    self.database = DEFAULT_DB_TABLE

    -- Bootstrap the schema on a brand-new DB. With upload_flag=false the
    -- construct path runs DROP/CREATE for every aux table; do this exactly
    -- once when the file doesn't exist yet.
    if not file_exists(db_path) then
        io.stderr:write(string.format(
            "PERSISTENCE: bootstrapping new db at %s\n", db_path))
        local cdt = CDT.new(db_path, self.database, nil, false)
        cdt:check_installation()
        cdt:disconnect()
    end

    -- Long-lived runtime handle for push_stream_data / set_status_data.
    self.rt = KBDS.new(db_path, self.database)

    -- (class .. "/" .. instance) -> state table
    self.instances = {}

    return self
end

-- Check whether a kb_name is already present in <database>_info.
function M:_kb_exists(kb_name)
    self.rt:clear_filters()
    self.rt:search_kb(kb_name)
    local rows = self.rt:execute_kb_search()
    return rows and #rows > 0
end

-- Apply a topology announcement. Returns (state, added, removed):
--   state  : the instance state (class, instance, kb_name, leaves)
--   added  : map of leaf-tail -> leaf-info for newly-announced entries
--   removed: map of leaf-tail -> leaf-info for entries no longer announced
-- The driver opens subs for `added` and closes subs for `removed`. On a
-- same-topology re-announce (robot reboot, periodic republish), both are
-- empty and the operation is a no-op.
function M:apply_topology(class, instance, entries)
    assert(type(class) == "string" and class ~= "", "class required")
    assert(type(instance) == "string" and instance ~= "", "instance required")
    assert(type(entries) == "table", "entries must be a list")

    local instance_key = class .. "/" .. instance
    local kb_name      = class .. "_" .. instance

    -- 1. Compute desired leaves from the announce.
    local desired = {}
    for _, e in ipairs(entries) do
        if e.kind == "stream" or e.kind == "status" then
            local field = tail_to_field(e.path)
            desired[e.path] = {
                tail       = e.path,
                full_key   = class .. "/" .. instance .. "/" .. e.path,
                kind       = e.kind,
                length     = e.length,
                desc       = e.desc,
                ltree_path = string.format("%s.%s.%s", kb_name, e.kind, field),
            }
        else
            io.stderr:write(string.format(
                "PERSISTENCE: skipping entry with unknown kind '%s' (path=%s)\n",
                tostring(e.kind), tostring(e.path)))
        end
    end

    -- 2. Diff desired vs existing (in-memory cache).
    local state    = self.instances[instance_key]
    local existing = (state and state.leaves) or {}
    local added, removed = {}, {}
    for tail, leaf in pairs(desired) do
        if not existing[tail] then added[tail] = leaf end
    end
    for tail, leaf in pairs(existing) do
        if not desired[tail] then removed[tail] = leaf end
    end

    -- 3. No-op short-circuit: same-topology re-announce of a known instance.
    if state and not next(added) and not next(removed) then
        return state, {}, {}
    end

    -- 4. Schema reconcile — additive only. We invoke construct_kb exactly
    -- for the ADDED leaves (so each one is declared once). REMOVED leaves
    -- stay in the DB.
    local kb_already = self:_kb_exists(kb_name)
    if next(added) or not kb_already then
        io.stderr:write(string.format(
            "PERSISTENCE: schema reconcile kb=%s (+%d new leaves%s)\n",
            kb_name, count_keys(added),
            kb_already and "" or ", new kb"))

        local cdt = CDT.new(self.db_path, self.database, nil, true)   -- upload_flag=true: never DROP
        if kb_already then
            -- A fresh CDT instance has empty in-memory path tracking. To call
            -- select_kb on a kb that already exists in kb_info, mirror what
            -- add_kb does internally — minus the kb_info INSERT, which would
            -- create a duplicate row. This is a deliberate bypass of the
            -- public add_kb (which errors on existing kbs).
            cdt.kb.path[kb_name]        = { kb_name }
            cdt.kb.path_values[kb_name] = {}
        else
            cdt:add_kb(kb_name, "auto-declared from persistence_topology announce")
        end
        cdt:select_kb(kb_name)
        for _tail, leaf in pairs(added) do
            local desc  = leaf.desc or leaf.tail
            local field = tail_to_field(leaf.tail)
            if leaf.kind == "stream" then
                cdt:add_stream_field(field, leaf.length or 100, desc)
            elseif leaf.kind == "status" then
                cdt:add_status_field(field, {}, desc, { value = nil })
            end
        end
        cdt:check_installation()
        cdt:disconnect()
    end

    if next(removed) then
        io.stderr:write(string.format(
            "PERSISTENCE: kb=%s — %d leaves removed from announce (data preserved)\n",
            kb_name, count_keys(removed)))
    end

    -- 5. Update in-memory state to reflect the announced topology.
    state = state or { class = class, instance = instance, kb_name = kb_name }
    state.leaves      = desired
    state.constructed = true
    self.instances[instance_key] = state

    return state, added, removed
end

-- Dispatch a received Zenoh sample on a known leaf.
function M:dispatch(leaf_info, payload)
    local ok, data = pcall(cjson.decode, payload)
    if not ok or type(data) ~= "table" then
        io.stderr:write(string.format(
            "PERSISTENCE: bad JSON on %s: %s\n",
            leaf_info.full_key, tostring(data)))
        return false
    end
    if leaf_info.kind == "stream" then
        local ok2, err = pcall(function()
            self.rt:push_stream_data(leaf_info.ltree_path, data)
        end)
        if not ok2 then
            io.stderr:write(string.format(
                "PERSISTENCE: push_stream_data %s: %s\n",
                leaf_info.ltree_path, tostring(err)))
            return false
        end
        return true
    elseif leaf_info.kind == "status" then
        local ok2, err = pcall(function()
            self.rt:set_status_data(leaf_info.ltree_path, data)
        end)
        if not ok2 then
            io.stderr:write(string.format(
                "PERSISTENCE: set_status_data %s: %s\n",
                leaf_info.ltree_path, tostring(err)))
            return false
        end
        return true
    end
    return false
end

function M:close()
    if self.rt then
        self.rt:disconnect()
        self.rt = nil
    end
end

return M
