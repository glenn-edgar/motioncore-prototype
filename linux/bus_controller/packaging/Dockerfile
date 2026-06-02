# syntax=docker/dockerfile:1.6
#
# bus_controller container — ONE process per logical dongle (the locked design).
# Connects to the fleet's zenohd router AS A CLIENT and serves the per-slave command
# RPC, bridging named-JSON commands through the LuaJIT wrapper + the portable C core
# to the SAMD21 RS-485 bus over USB-CDC.
#
# This is a CLIENT of the router — no zenohd/sqlite here, so the image stays lean.
# debian:bookworm (glibc 2.36) is required by the prebuilt zenoh-pico .so.
#
# Build: from linux/bus_controller/, run packaging/build.sh (stages the zenoh assets
#        then docker build). Run: needs --device for the dongle + the router reachable
#        (--network=host against the fleet container's zenohd).

# ---- stage 1: build the portable C core as a shared lib --------------------
FROM debian:bookworm-slim AS builder
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev make \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY *.c *.h Makefile ./
COPY vendor ./vendor
RUN make libbus_controller.so

# ---- stage 2: lean runtime ------------------------------------------------
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        luajit libssl3 tini ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# zenoh-pico + the FFI shim C libs (staged by build.sh)
COPY packaging/build_assets/lib/*.so /usr/local/lib/
RUN ldconfig

# the portable C core (built above), the zenoh lua bindings, the bus lua + rosters
COPY --from=builder /build/libbus_controller.so /app/libbus_controller.so
COPY packaging/build_assets/zlib/*.lua          /app/zlib/
COPY lua/*.lua                                  /app/lua/
COPY rosters                                    /app/rosters
COPY packaging/entrypoint.sh                    /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh

# Sane defaults (override with `docker run -e ...`).
ENV BUS_DEVICE=/dev/ttyACM0 \
    ROUTER=tcp/127.0.0.1:7447 \
    ROSTER=/app/rosters/bench.conf

# tini = PID 1 + signal forwarding. CMD selects mode (service | client).
ENTRYPOINT ["/usr/bin/tini", "--", "/app/entrypoint.sh"]
CMD ["service"]
