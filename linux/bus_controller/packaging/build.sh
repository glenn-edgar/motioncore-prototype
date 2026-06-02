#!/bin/sh
# Stage the prebuilt zenoh assets (zenoh-pico + the LuaJIT FFI shim libs/bindings)
# into packaging/build_assets/, then build the bus_controller image. Run from
# anywhere; paths are resolved relative to this script.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"      # .../packaging
BC="$(dirname "$HERE")"                     # the build context (bus_controller dir)
# REPO holds the zenoh assets. Defaults to the full repo clone; override with REPO=...
# (the Pi keeps a flat ~/bus_controller deploy copy separate from ~/motioncore-prototype).
REPO="${REPO:-$HOME/motioncore-prototype}"
ZL="$REPO/zenoh_libs/luajit"
PICO="$REPO/fleet_design/packaging/build_assets/lib/libzenohpico.so"
ASSETS="$HERE/build_assets"
TAG="${1:-bus_controller:0.1}"

mkdir -p "$ASSETS/lib" "$ASSETS/zlib"

# C libs: zenoh-pico + the three FFI shim libs (rpc/token/pubsub)
cp "$PICO"                                          "$ASSETS/lib/"
cp "$ZL/libzenoh_rpc.so" "$ZL/libzenoh_token.so" "$ZL/libzenoh_pubsub.so" "$ASSETS/lib/"
# LuaJIT bindings for those libs
cp "$ZL/lib/zenoh_rpc.lua" "$ZL/lib/zenoh_token.lua" "$ZL/lib/zenoh_pubsub.lua" "$ASSETS/zlib/"

echo "[build] staged zenoh assets into $ASSETS"
echo "[build] docker build -> $TAG (context $BC)"
cd "$BC"
docker build -f packaging/Dockerfile -t "$TAG" .
echo "[build] done: $TAG"
