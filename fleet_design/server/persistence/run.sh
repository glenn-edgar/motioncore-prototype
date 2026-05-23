#!/usr/bin/env bash
# run.sh — launcher for the persistence layer of the robot controller.
#
# Optional env: ZENOH_LOCATOR  (default tcp/127.0.0.1:7447)
#               SERVICE_ID     (default persistence-1)
#               PERSISTENCE_DB (default /tmp/persistence.db)
#
# Mirrors fleet_manager/run.sh + adds the build step for ltree.so on first
# run (sqlite3 load_extension finds /usr/local/lib/ltree.so via its default
# search path).

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
VENDOR_LUA=$REPO_ROOT/vendor/lua

# LuaJIT-compatible cjson — must precede the user's Lua-5.4-ABI luarocks copy.
: "${LUA_CPATH:=/usr/local/lib/lua/5.1/?.so;;}"
export LUA_CPATH

export LUA_PATH="$VENDOR_LUA/?.lua;$SCRIPT_DIR/lib/?.lua;$SCRIPT_DIR/?.lua;;"

# TODO Pi/container deploy: replace bench LD_LIBRARY_PATH with vendor/lib-*.
if [ -z "${LD_LIBRARY_PATH:-}" ]; then
    DEFAULT_ZENOH_LIB=$HOME/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/knowledge_base/zenoh
    DEFAULT_PICO_LIB=$HOME/src/zenoh-pico/lib-combined
    export LD_LIBRARY_PATH="$DEFAULT_ZENOH_LIB:$DEFAULT_PICO_LIB"
fi

# Build & install ltree.so if it isn't on the host yet (Dockerfile does the
# equivalent at image build; this branch is for bench / Pi-bare-process).
if [ ! -f /usr/local/lib/ltree.so ] && [ ! -f ./ltree.so ]; then
    echo "persistence/run.sh: building vendored ltree.so" >&2
    (cd "$REPO_ROOT/vendor/c/ltree" && make && sudo make install)
fi

exec luajit "$SCRIPT_DIR/main.lua" "$@"
