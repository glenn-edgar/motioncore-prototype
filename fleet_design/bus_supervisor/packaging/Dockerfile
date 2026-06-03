# syntax=docker/dockerfile:1.6
#
# bus_supervisor container — Slice 3: a SELF-CONTAINED, dedicated image.
#
# Runs its OWN router (zenohd on 0.0.0.0:7448, multicast off — isolated from any
# other fleet router) AND the chain_tree one_for_one bus supervisor, both under
# tini. At start it builds the IR from the bind-mounted /configs (Idea A: drop a
# config + restart = reconfigured), then serves each dongle's command RPC over the
# LAN, bridging named-JSON commands through the LuaJIT runtime + the portable C
# core to the SAMD21 RS-485 bus over USB-CDC.
#
# Build context = this packaging/ dir; build.sh stages everything into
# build_assets/ first (C source, vendor lua, the bus_supervisor app, lua_dsl,
# rosters). Binaries (zenohd, zenoh-pico .so, cjson) come from the known-good
# fleet image so the ABI matches what we already run.
#
# Run: needs --device for the dongle(s), -v <dir>:/configs:ro for the per-dongle
#      JSON, and host networking so :7448 is reachable from the LAN (WSL clients).

# ---- binary source: zenohd + zenoh-pico .so + luajit cjson (known-good) -----
FROM nanodatacenter/fleet-mcfarland:0.7 AS deps

# ---- stage 1: build the portable C core as a shared lib ---------------------
FROM debian:bookworm-slim AS builder
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev make \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY build_assets/csrc/ ./
RUN make libbus_controller.so

# ---- stage 2: lean runtime --------------------------------------------------
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        luajit libssl3 tini ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# router + client transport + json, all from the known-good fleet image
COPY --from=deps /usr/bin/zenohd                       /usr/bin/zenohd
COPY --from=deps /usr/local/lib/libzenoh_pubsub.so     /usr/local/lib/
COPY --from=deps /usr/local/lib/libzenoh_rpc.so        /usr/local/lib/
COPY --from=deps /usr/local/lib/libzenoh_token.so      /usr/local/lib/
COPY --from=deps /usr/local/lib/libzenohpico.so        /usr/local/lib/
COPY --from=deps /usr/local/lib/lua/5.1/cjson.so       /usr/local/lib/lua/5.1/cjson.so
RUN ldconfig

# the portable C core (built above)
COPY --from=builder /build/libbus_controller.so        /app/libbus_controller.so

# the LuaJIT app: chain_tree runtime + zenoh bindings, the bus_supervisor program,
# the DSL builder (for build-at-start), and the rosters
COPY build_assets/vendor_lua/      /app/vendor_lua/
COPY build_assets/app/             /app/bus_supervisor/
COPY build_assets/lua_dsl/         /app/lua_dsl/
COPY build_assets/rosters/         /app/rosters/
COPY start.sh                      /app/start.sh
RUN chmod +x /app/start.sh

# Defaults (override with `docker run -e ...`). Configs are bind-mounted at
# /configs; each config's roster field points at a baked /app/rosters/<file>.
ENV BUS_SUP_CONFIGS=/configs \
    BUS_SUP_DSL=/app/lua_dsl/ \
    BUS_SUP_ROUTER_PORT=7448 \
    BUS_SUP_TICK_HZ=2000 \
    BUS_LIB=/app/libbus_controller.so

# tini = PID 1 + signal forwarding; start.sh launches the router + supervisor.
ENTRYPOINT ["/usr/bin/tini", "--", "/app/start.sh"]
