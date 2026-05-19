#!/usr/bin/env bash
# run.sh — launcher for fake_robot using only repo-relative paths.
#
# Required env:   ROBOT_CLASS, ROBOT_INSTANCE
# Optional env:   IDENTITY_DIR (default ./identity), ZENOH_LOCATOR
#                 (default tcp/127.0.0.1:7447), FAKE_ROBOT_TICK_HZ (default 10)
#
# Native .so loader path is still external for bench dev. Pi Zero 2 deploy
# will copy aarch64 builds into vendor/lib-aarch64/ and this script will
# point LD_LIBRARY_PATH there instead — see TODO below.

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
VENDOR_LUA=$REPO_ROOT/vendor/lua

# LuaJIT-compatible cjson (system /usr/local/lib/lua/5.1) — must come before
# the user's ~/.luarocks (which has a Lua-5.4-ABI cjson).
: "${LUA_CPATH:=/usr/local/lib/lua/5.1/?.so;;}"
export LUA_CPATH

# Repo-relative LUA_PATH only. No reference to upstream knowledge_base_assembly.
export LUA_PATH="$VENDOR_LUA/?.lua;$SCRIPT_DIR/lib/?.lua;$SCRIPT_DIR/?.lua;$SCRIPT_DIR/chains/?.lua;;"

# TODO Pi deploy: replace bench LD_LIBRARY_PATH with vendor/lib-aarch64/.
# Bench dev expects libzenoh_*.so + libzenohpico.so on the loader path.
# Allow caller to override LD_LIBRARY_PATH entirely; only set defaults if unset.
if [ -z "${LD_LIBRARY_PATH:-}" ]; then
    DEFAULT_ZENOH_LIB=$HOME/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/knowledge_base/zenoh
    DEFAULT_PICO_LIB=$HOME/src/zenoh-pico/lib-combined
    export LD_LIBRARY_PATH="$DEFAULT_ZENOH_LIB:$DEFAULT_PICO_LIB"
fi

exec luajit "$SCRIPT_DIR/main.lua" "$@"
