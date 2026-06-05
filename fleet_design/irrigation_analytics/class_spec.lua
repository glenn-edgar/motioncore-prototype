-- class_spec.lua — irrigation_analytics class spec.
--
-- The robot's job: real-time monitor + analyzer of the LaCima irrigation
-- controller. Mirrors the existing bare-LuaJIT robot at robot/main.lua, but
-- packaged into the fleet_design chain_tree framework so it gets:
--   - Zenoh persistence of state + events (queryable history)
--   - Dashboard panel via application_gateway
--   - Manual suspend RPC from the web portal
--   - Uniform heartbeat / Discord pattern
--
-- Phase 1: skeleton — single "monitor" app KB that just heartbeats. The
-- KB1/KB3 detection logic ports in Phase 3; KB2 calibrator in Phase 4.

local M = {}

M.capabilities = {
    "heartbeat",
    "irrigation_state",
    "irrigation_fault_detection",
}

M.app_kbs = { "monitor", "detector", "kb4_clog" }

-- Controller config — where to fetch popup + past_actions from.
-- Reused by lib/controller_client.lua (which wraps the SSH+python popup
-- fetch from the existing robot/lib/controller.lua).
M.controller = {
    ssh_host  = os.getenv("IRRIGATION_CONTROLLER_HOST") or "pi@irrigation",
    timeout_s = 8,
    poll_s    = tonumber(os.getenv("IRRIGATION_POLL_S") or "30"),
}

-- Detector config — KB1+KB3 fault detector. Same SSH/poll knobs as controller
-- by default; can be split per-env if KB1 needs a different cadence.
-- curves_path  → per-bin KB1 current thresholds (output of explore/generate_curves)
-- baselines_path → per-bin KB3 flow baselines (output of explore/baseline_state/)
-- Both shipped into the image at build time; runtime is read-only for KB1/KB3.
-- KB2/KB4 (when added) will need a bind-mount instead since they write.
M.detector = {
    ssh_host       = os.getenv("IRRIGATION_CONTROLLER_HOST") or "pi@irrigation",
    timeout_s      = 8,
    poll_s         = tonumber(os.getenv("IRRIGATION_DETECTOR_POLL_S") or "30"),
    curves_path    = os.getenv("KB1_THRESHOLDS_JSON")
                     or "/app/irrigation_analytics/data/kb1_thresholds.json",
    baselines_path = os.getenv("BASELINES_JSON")
                     or "/app/irrigation_analytics/data/baselines.json",
}

-- KB4 clog/leak detector config — flow-only, per-bin SQLite baseline.
-- Subscribes to past_actions STEP_COMPLETE, classifies non-ETO bins,
-- emits Discord on LEAK (flow > baseline+5 GPM), DB-warn on ±3 GPM drift.
-- ETO bins are skipped (deferred to a separate ETO-path handler).
-- db_path lives on the writable bind mount; seed + eto_valves are baked
-- into the image.
M.kb4_clog = {
    ssh_host         = os.getenv("IRRIGATION_CONTROLLER_HOST") or "pi@irrigation",
    timeout_s        = 8,
    poll_s           = tonumber(os.getenv("IRRIGATION_KB4_POLL_S") or "30"),
    db_path          = os.getenv("KB4_DB_PATH") or "/var/fleet/kb4/kb4.db",
    seed_path        = os.getenv("KB4_SEED_JSON")
                       or "/app/irrigation_analytics/data/kb4_nonETO_baselines.json",
    seed_eto_path    = os.getenv("KB4_ETO_SEED_JSON")
                       or "/app/irrigation_analytics/data/kb4_ETO_baselines.json",
    eto_valves_path  = os.getenv("ETO_VALVES_JSON")
                       or "/app/irrigation_analytics/data/eto_valves.json",
}

-- Persistence-topology declaration. Three leaves for the v1 skeleton:
--   state/latest  — UPSERT status; current irrigation state snapshot
--   events/sample — append-only stream of KB1/KB3 fires (Phase 3+)
--   heartbeat     — rolled up via KB0
--
-- valve_test/{latest,sample} get added in Phase 4 when the KB2
-- calibrator KB lands.
function M.persistence_topology()
    return {
        { path = "state/latest",
          kind = "status",
          desc = "current irrigation state (schedule, step, master, currents)" },
        { path   = "events/sample",
          kind   = "stream",
          length = 200,
          desc   = "KB1/KB3 fire events (level/kind/action/msg per fire)" },
        { path = "heartbeat",
          kind = "status",
          desc = "robot rolled-up heartbeat" },
    }
end

-- Publish persistence_topology on namespace_up + periodically. Mirrors
-- rancho_water/farm_soil pattern — could share via robot_common later.
function M.publish_persistence_topology(ps, identity, silent)
    local cjson = require("cjson")
    local topo = M.persistence_topology()
    local payload = cjson.encode({
        schema   = "persistence_topology/1",
        class    = identity.class,
        instance = identity.instance,
        entries  = topo,
    })
    local function pub(key)
        local ok, err = pcall(function() ps:publish(key, payload) end)
        if not ok then
            io.stderr:write(string.format(
                "IRRIGATION_ANALYTICS [%s]: persistence_topology publish to %s failed: %s\n",
                identity.namespace, key, tostring(err)))
        end
        return ok
    end
    local ok1 = pub(identity.namespace .. "/persistence_topology")
    local ok2 = pub("fleet/admin/persistence_topology_announce")
    if ok1 and ok2 and not silent then
        io.stderr:write(string.format(
            "IRRIGATION_ANALYTICS [%s]: persistence_topology published (%d entries, 2 channels)\n",
            identity.namespace, #topo))
    end
    return ok1 and ok2
end

M.PERSISTENCE_TOPOLOGY_REPUBLISH_S = 30

function M.on_namespace_up(ps, identity, bb)
    M.publish_persistence_topology(ps, identity, false)
end

return M
