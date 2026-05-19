#!/usr/bin/env bash
# run.sh — launcher for the throwaway bench_manager stub controller.
#
# Optional env: ZENOH_LOCATOR (default tcp/127.0.0.1:7447),
#               CONTROLLER_ID (default "bench-stub-1").
#
# Same LUA_PATH / LD_LIBRARY_PATH discipline as fake_robot/run.sh — no
# upstream knowledge_base_assembly references; everything Lua-side comes
# from vendor/lua/.

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
VENDOR_LUA=$REPO_ROOT/vendor/lua

: "${LUA_CPATH:=/usr/local/lib/lua/5.1/?.so;;}"
export LUA_CPATH

export LUA_PATH="$VENDOR_LUA/?.lua;$SCRIPT_DIR/?.lua;;"

# TODO Pi deploy: replace bench LD_LIBRARY_PATH with vendor/lib-aarch64/.
if [ -z "${LD_LIBRARY_PATH:-}" ]; then
    DEFAULT_ZENOH_LIB=$HOME/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/knowledge_base/zenoh
    DEFAULT_PICO_LIB=$HOME/src/zenoh-pico/lib-combined
    export LD_LIBRARY_PATH="$DEFAULT_ZENOH_LIB:$DEFAULT_PICO_LIB"
fi

exec luajit "$SCRIPT_DIR/main.lua" "$@"
