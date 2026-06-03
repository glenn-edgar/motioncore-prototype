#!/bin/sh
# /app/start.sh — bus_supervisor self-contained startup (tini's child, PID 2).
#
# 1) launch this stack's OWN zenohd router (0.0.0.0:<port>, multicast off so it
#    never gossip-merges with another fleet's router),
# 2) build the chain_tree IR from the bind-mounted /configs (build-at-start —
#    drop a config + restart = reconfigured),
# 3) exec the supervisor, which connects to the local router and serves each
#    dongle's command RPC.
#
# If the router OR the supervisor exits, we exit non-zero so the docker daemon's
# --restart=unless-stopped recreates the whole container (whole-stack restart;
# the chain_tree one_for_one supervisor handles per-dongle faults internally).
set -u

export LUA_CPATH="/usr/local/lib/lua/5.1/?.so;;"
export LD_LIBRARY_PATH="/usr/local/lib:/app"
export BUS_LIB="${BUS_LIB:-/app/libbus_controller.so}"

APP=/app/bus_supervisor
CONFIGS="${BUS_SUP_CONFIGS:-/configs}"
PORT="${BUS_SUP_ROUTER_PORT:-7448}"
export BUS_SUP_CONFIGS="$CONFIGS"
export BUS_SUP_DSL="${BUS_SUP_DSL:-/app/lua_dsl/}"

# runtime LUA_PATH: chain_tree runtime + zenoh bindings, then the app trees/lib
RUN_PATH="/app/vendor_lua/?.lua;$APP/lib/?.lua;$APP/?.lua;$APP/chains/?.lua;;"

# ---- 1) own router ----------------------------------------------------------
echo "[bus_sup] starting own router: zenohd -l tcp/0.0.0.0:${PORT} (multicast off)"
zenohd -l "tcp/0.0.0.0:${PORT}" --cfg=scouting/multicast/enabled:false &
ZENOHD_PID=$!
sleep 2
if ! kill -0 "$ZENOHD_PID" 2>/dev/null; then
    echo "[bus_sup] zenohd failed to start — aborting"; exit 1
fi

# ---- 2) build the IR from the mounted configs -------------------------------
echo "[bus_sup] building IR from configs in ${CONFIGS}"
# the DSL build needs lua_dsl + lua_support on the path in addition to the app
LUA_PATH="$APP/chains/?.lua;$APP/lib/?.lua;/app/lua_dsl/?.lua;/app/lua_dsl/lua_support/?.lua;/app/vendor_lua/?.lua;;" \
    luajit "$APP/chains/build.lua" "$APP/chains/bus_sup.json" || {
        echo "[bus_sup] IR build FAILED — check that ${CONFIGS} has at least one *.json"
        kill -TERM "$ZENOHD_PID" 2>/dev/null; exit 1; }

# ---- 3) run the supervisor against the local router -------------------------
export ROUTER="tcp/127.0.0.1:${PORT}"
echo "[bus_sup] launching supervisor (router ${ROUTER})"
export LUA_PATH="$RUN_PATH"
luajit "$APP/main.lua" &
SUP_PID=$!

# ---- 4) supervise both: exit (→ container restart) when EITHER dies ---------
# POSIX sh has no `wait -n`; poll. The chain_tree one_for_one supervisor handles
# per-dongle faults internally — this only catches a router/supervisor death.
while kill -0 "$ZENOHD_PID" 2>/dev/null && kill -0 "$SUP_PID" 2>/dev/null; do
    sleep 1
done
echo "[bus_sup] router or supervisor exited — stopping container"
kill -TERM "$ZENOHD_PID" "$SUP_PID" 2>/dev/null
wait "$SUP_PID" 2>/dev/null
exit 1
