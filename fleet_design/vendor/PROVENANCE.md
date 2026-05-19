# vendor/ — Third-party runtime files vendored into fleet_design

These files are **copies** from upstream Lua-side projects under
`~/knowledge_base_assembly/`. They live here so a partial-repo pull
(e.g., to a Pi Zero 2) gets everything the fake_robot / future Linux
robots need at runtime, with no reference to the upstream tree.

Refresh: re-copy from upstream when you intentionally pick up a new
version. Suggested workflow is `tools/vendor_refresh.sh` (not yet
written — manual `cp` for now per the table below).

## File map

| Vendored file | Upstream source | Purpose |
|---|---|---|
| `lua/ct_loader.lua` | `chain_tree_luajit/runtime_dict/ct_loader.lua` | Load compiled JSON IR into runtime handle |
| `lua/ct_runtime.lua` | `chain_tree_luajit/runtime_dict/ct_runtime.lua` | KB lifecycle + handle factory |
| `lua/ct_engine.lua` | `chain_tree_luajit/runtime_dict/ct_engine.lua` | Node execution / event dispatch |
| `lua/ct_builtins.lua` | `chain_tree_luajit/runtime_dict/ct_builtins.lua` | Built-in node functions (CFL_COLUMN_*, watchdog, log, state machine, ...) |
| `lua/ct_definitions.lua` | `chain_tree_luajit/runtime_dict/ct_definitions.lua` | Return-code + event-id enums (CFL_TIMER_EVENT, CFL_SECOND_EVENT, etc.) |
| `lua/ct_common.lua` | `chain_tree_luajit/runtime_dict/ct_common.lua` | Tree helpers (children, parents) |
| `lua/ct_walker.lua` | `chain_tree_luajit/runtime_dict/ct_walker.lua` | Iterative DFS walker |
| `lua/fn_registry.lua` | `ros_planner_ii/runtime/fn_registry.lua` | Maps user-fn names from compiled IR to Lua callables |
| `lua/zenoh_pubsub.lua` | `knowledge_base/zenoh/lib/zenoh_pubsub.lua` | LuaJIT FFI binding for libzenoh_pubsub |
| `lua/zenoh_rpc.lua` | `knowledge_base/zenoh/lib/zenoh_rpc.lua` | LuaJIT FFI binding for libzenoh_rpc |
| `lua/zenoh_token.lua` | `knowledge_base/zenoh/lib/zenoh_token.lua` | LuaJIT FFI binding for libzenoh_token (FNV1a-32 topic hash) |

Upstream root on dev: `~/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/`

## What is NOT vendored

- **`chain_tree_luajit/lua_dsl/`** — DSL builder. Build-time only; runs on the
  dev machine to regenerate `connection.json` from `connection.lua`. The Pi
  consumes the compiled IR; it never builds.
- **Native shared libraries** (`libzenoh_pubsub.so`, `libzenoh_rpc.so`,
  `libzenoh_token.so`, `libzenohpico.so`). Per-arch builds. Bench dev uses
  external `LD_LIBRARY_PATH`; Pi Zero 2 deploy will need aarch64 builds
  copied into `vendor/lib-aarch64/` (directory not yet created — wait until
  the deploy work begins).

## Freshness

| Date | Action |
|---|---|
| 2026-05-19 | Initial vendor copy from local upstream. |
