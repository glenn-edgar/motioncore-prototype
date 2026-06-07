#!/bin/bash
# packaging_irrigation_analytics/build.sh — stage prebuilt .so files (shared
# with the fleet-mcfarland image's build_assets) and build the
# irrigation-analytics container image.
#
# Today: native arm64 on the WSL host (Apple Silicon class) — same arch as
# Pi 4 and Arduino Uno Q. Image is portable to either without buildx.

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
SHARED_PACKAGING_DIR="$REPO_ROOT/packaging"
ASSETS_DIR="$SHARED_PACKAGING_DIR/build_assets"
LIB_DIR="$ASSETS_DIR/lib"

# Where the .so files live on the bench. Override with env if elsewhere.
ZENOH_PICO_DIR=${ZENOH_PICO_DIR:-$HOME/src/zenoh-pico/lib-combined}
ZENOH_SHIM_DIR=${ZENOH_SHIM_DIR:-$HOME/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/knowledge_base/zenoh}

ZENOH_RPC_SO=${ZENOH_RPC_SO:-$ZENOH_SHIM_DIR/libzenoh_rpc.so}
ZENOH_PUBSUB_SO=${ZENOH_PUBSUB_SO:-$ZENOH_SHIM_DIR/libzenoh_pubsub.so}
ZENOH_TOKEN_SO=${ZENOH_TOKEN_SO:-$ZENOH_SHIM_DIR/libzenoh_token.so}

IMAGE_TAG=${IMAGE_TAG:-nanodatacenter/irrigation-analytics:wsl}

echo "==> Staging prebuilt zenoh .so files into $LIB_DIR"
mkdir -p "$LIB_DIR"
rm -f "$LIB_DIR"/*.so

for src in \
    "$ZENOH_PICO_DIR/libzenohpico.so" \
    "$ZENOH_PUBSUB_SO" \
    "$ZENOH_RPC_SO" \
    "$ZENOH_TOKEN_SO" \
    ; do
    if [ ! -f "$src" ]; then
        echo "ERROR: missing $src" >&2
        echo "Set ZENOH_PICO_DIR / ZENOH_SHIM_DIR / ZENOH_*_SO to override." >&2
        exit 1
    fi
    cp "$src" "$LIB_DIR/"
done

ls -la "$LIB_DIR"

echo "==> docker build -t $IMAGE_TAG (context=$REPO_ROOT)"
cd "$REPO_ROOT"
docker build -f packaging_irrigation_analytics/Dockerfile -t "$IMAGE_TAG" .

echo "==> done. image: $IMAGE_TAG"
docker images "$IMAGE_TAG"
