#!/bin/sh
# env for running the LuaJIT bus service/client against the container's zenohd router
REPO="$HOME/motioncore-prototype"
ZL="$REPO/zenoh_libs/luajit"
export LUA_PATH="$ZL/lib/?.lua;./lua/?.lua;;"
export LD_LIBRARY_PATH="$ZL:$REPO/fleet_design/packaging/build_assets/lib:."
export BUS_LIB="./libbus_controller.so"
exec luajit "$@"
