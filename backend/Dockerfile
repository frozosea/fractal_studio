FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake libopencv-dev libsqlite3-dev libssl-dev libomp-dev \
    libmpfr-dev libgmp-dev libquadmath0 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY backend /src/backend
RUN cmake -S backend -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j2

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libopencv-core406 libopencv-imgcodecs406 libopencv-imgproc406 libopencv-videoio406 \
    libsqlite3-0 libssl3 libgomp1 libmpfr6 libquadmath0 ffmpeg curl \
    && rm -rf /var/lib/apt/lists/*
ENV FSD_ENABLE_LEGACY_API=0 \
    FSD_ENABLE_LEGACY_FORMULA_COMPILER=0 \
    FSD_STARTUP_BENCHMARK=off
WORKDIR /workspace/fractal_studio
COPY --from=build /src/build/fractal_studio_backend /usr/local/bin/fractal_studio_backend
COPY frontend/package.json /workspace/fractal_studio/frontend/package.json
COPY backend/CMakeLists.txt /workspace/fractal_studio/backend/CMakeLists.txt
RUN groupadd --system fractal \
    && useradd --system --gid fractal --home-dir /workspace/fractal_studio fractal \
    && mkdir -p /workspace/fractal_studio/runtime \
    && chown -R fractal:fractal /workspace/fractal_studio
USER fractal
EXPOSE 18080
CMD ["fractal_studio_backend", "18080"]
