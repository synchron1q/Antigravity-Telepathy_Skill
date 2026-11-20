# Multi-stage reproducible build for ag-mesh (Relay Coordinator or Test Cluster Node)
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git pkg-config libssl-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake --preset linux-release -DAGMESH_BUILD_TUI=OFF -DAGMESH_BUILD_TESTS=ON \
    && cmake --build --preset linux-release --parallel \
    && ctest --preset linux-release --output-on-failure

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3t64 ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/linux-release/ag-mesh /usr/local/bin/ag-mesh
COPY --from=builder /src/skill/antigravity-mesh /usr/local/share/ag-mesh/skill/antigravity-mesh

RUN useradd -r -m -s /sbin/nologin agmesh
USER agmesh
WORKDIR /home/agmesh

EXPOSE 47890 47895
HEALTHCHECK --interval=15s --timeout=3s --start-period=5s \
    CMD curl -fsS http://127.0.0.1:47895/healthz || exit 1

ENTRYPOINT ["/usr/local/bin/ag-mesh"]
CMD ["daemon"]
