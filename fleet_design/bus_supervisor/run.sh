#!/usr/bin/env bash
# run.sh — launcher for the bus supervisor (chain_tree), repo-relative paths.
#
# Optional env: BUS_SUP_TICK_HZ (default 10), BUS_SUP_MAX_S (default 0 = forever).
# Slice 1 needs no native .so (dongle lifecycle stubbed); slice 1b adds the zenoh
# + libbus_controller.so loader path here.

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)   # fleet_design/
VENDOR_LUA=$REPO_ROOT/vendor/lua

# LuaJIT-compatible cjson (system 5.1) — ct_loader parses the JSON IR.
: "${LUA_CPATH:=/usr/local/lib/lua/5.1/?.so;;}"
export LUA_CPATH

export LUA_PATH="$VENDOR_LUA/?.lua;$SCRIPT_DIR/lib/?.lua;$SCRIPT_DIR/?.lua;$SCRIPT_DIR/chains/?.lua;$REPO_ROOT/robot_common/lib/?.lua;$REPO_ROOT/robot_common/chains/?.lua;;"

exec luajit "$SCRIPT_DIR/main.lua" "$@"
