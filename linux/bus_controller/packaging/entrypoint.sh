#!/bin/sh
# bus_controller container entrypoint. Sets the in-image lua/lib paths, then runs
# either the bus SERVICE (default — holds the dongle, serves the per-slave cmd RPC)
# or the CLIENT test (for in-container verification).
set -e

export LUA_PATH="/app/zlib/?.lua;/app/lua/?.lua;;"
export LD_LIBRARY_PATH="/usr/local/lib:/app"
export BUS_LIB="/app/libbus_controller.so"

MODE="${1:-service}"
case "$MODE" in
  client)
    exec luajit /app/lua/zclient_capstone.lua "${2:-12}"
    ;;
  supervisor)
    echo "[bus_controller] supervisor: router=${ROUTER}"
    exec luajit /app/lua/bus_supervisor.lua
    ;;
  service|*)
    echo "[bus_controller] device=${BUS_DEVICE} router=${ROUTER} roster=${ROSTER}"
    exec luajit /app/lua/bus_service.lua "${BUS_DEVICE}"
    ;;
esac
