#!/bin/sh
# Stage everything the bus_supervisor image needs into packaging/build_assets/,
# then docker build. The assets span three repo trees (linux/bus_controller C
# core, fleet_design/vendor chain_tree runtime, the bus_supervisor app) plus the
# external lua_dsl checkout — staging keeps the docker build context small.
#
# Run from anywhere. Override: TAG=bus_supervisor:0.2  DSL=/path/to/lua_dsl  ./build.sh
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"          # .../bus_supervisor/packaging
BUS_SUP="$(dirname "$HERE")"                    # .../bus_supervisor
FLEET="$(dirname "$BUS_SUP")"                   # .../fleet_design
REPO="$(dirname "$FLEET")"                      # repo root
TAG="${1:-${TAG:-bus_supervisor:0.1}}"
DSL="${DSL:-$HOME/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/chain_tree_luajit/lua_dsl}"

CSRC="$REPO/linux/bus_controller"
A="$HERE/build_assets"

[ -f "$CSRC/Makefile" ] || { echo "missing C core at $CSRC"; exit 1; }
[ -d "$DSL/lua_support" ] || { echo "missing lua_dsl at $DSL (set DSL=...)"; exit 1; }

echo "[build] staging assets into $A"
rm -rf "$A"
mkdir -p "$A/csrc" "$A/vendor_lua" "$A/app" "$A/lua_dsl" "$A/rosters"

# 1) C core (compiled in the builder stage): sources + Makefile + its vendor/
cp "$CSRC"/*.c "$CSRC"/*.h "$CSRC/Makefile" "$A/csrc/"
cp -r "$CSRC/vendor" "$A/csrc/vendor"

# 2) chain_tree runtime + zenoh lua bindings
cp "$FLEET"/vendor/lua/*.lua "$A/vendor_lua/"

# 3) the bus_supervisor app (NOT configs — bind-mounted; NOT packaging/build_assets)
cp "$BUS_SUP/main.lua" "$BUS_SUP/run.sh" "$A/app/"
cp -r "$BUS_SUP/lib" "$BUS_SUP/chains" "$BUS_SUP/tools" "$A/app/"
rm -f "$A/app/chains/bus_sup.json"             # rebuilt in-container at start

# 4) the DSL builder (build-at-start) + rosters
cp -r "$DSL/." "$A/lua_dsl/"
cp "$CSRC"/rosters/*.conf "$A/rosters/"

echo "[build] docker build -> $TAG (context $HERE)"
cd "$HERE"
docker build -t "$TAG" .
echo "[build] done: $TAG"
