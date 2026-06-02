-- chains/bus_sup.lua — the bus supervisor KB (chain_tree).
--
-- The fleet_manager bus role as an Erlang-style one_for_one supervision tree
-- (Model 1: in-process subtrees). One supervised child per logical dongle; each
-- child is the dongle lifecycle: bind (claim the unique BC) -> serve (pump bus +
-- RPC + announce) with term-on-disable cleanup. A child that faults disables
-- itself (running its term_fn → flock/controller release), and the one_for_one
-- supervisor re-enables it (→ init_fn → re-bind). Leaky-bucket restart limiting
-- (max_reset_number / reset_window) escalates a flapping dongle to BUS_GATE_DOWN.
--
-- Slice 1: bind/verify/serve are STUBS in bus_sup_user_functions.lua (dev-host
-- supervision proof). Slice 1b swaps in the real FFI (libbus_controller.so +
-- zenoh). The KB shape does not change.
--
-- DSL builder method ref: define_supervisor_one_for_one_node(
--   name, aux, data, restart_enabled, reset_limited_enabled,
--   max_reset_number, reset_window, auto_start, finalize_fn, finalize_data)

local KB_NAME = "bus_supervisor"

-- One supervised child = one dongle lifecycle column.
--   main = DONGLE_SERVE  (each tick: pump/serve/announce; CFL_DISABLE on fault)
--   init = DONGLE_BIND   (claim the unique BC + verify topology)
--   term = DONGLE_TERM   (release flock + close controller — runs on every disable)
local function add_dongle(ct, cfg)
    local d = ct:define_column("dongle:" .. cfg.dongle_id,
        "DONGLE_SERVE", "DONGLE_BIND", "DONGLE_TERM", nil, cfg, true)
    ct:end_column(d)
    return d
end

-- dongles = array of per-dongle config tables { dongle_id=..., [fail_after=...] }
local function build_kb(ct, kb_name, dongles)
    ct:start_test(kb_name)

    -- The first element of a KB must be a column.
    local outer = ct:define_column("bus_sup_outer", nil, nil, nil, nil, {}, true)
        ct:asm_log_message("BUS_SUP: starting one_for_one supervisor")

        -- restart_enabled=true, reset_limited_enabled=true (bootloop guard),
        -- max_reset_number=3 failures within reset_window TICKS, auto_start=true,
        -- finalize=BUS_GATE_DOWN. reset_window=120000 ticks ≈ 60 s @2 kHz pump —
        -- catches a dongle that keeps failing (each fail/rebind cycle is seconds)
        -- without false-tripping on a lone transient that rebinds cleanly.
        local sup = ct:define_supervisor_one_for_one_node(
            "bus_sup", "CFL_NULL", {}, true, true, 3, 120000, true, "BUS_GATE_DOWN", {})
            for _, cfg in ipairs(dongles) do add_dongle(ct, cfg) end
        ct:end_column(sup)

        -- Terminal halt — only reached if the supervisor disables (gate down);
        -- keeps the KB (and the process) alive so the gate stays observable.
        ct:asm_halt()
    ct:end_column(outer)

    ct:end_test()
end

return { build_kb = build_kb, KB_NAME = KB_NAME, add_dongle = add_dongle }
