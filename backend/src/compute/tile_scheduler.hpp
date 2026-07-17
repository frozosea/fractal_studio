// compute/tile_scheduler.hpp
//
// Hybrid CPU+GPU tile scheduler.
//
// A viewport is split into tiles (default 96×96). Tiles are pushed into a
// shared work queue. Two worker pools draw from the queue:
//
//   CPU pool — one worker per visible logical core by default.
//              Runs the AVX-512 or OpenMP path depending on availability.
//
//   GPU pool — one worker. Runs CUDA tiles through the CUDA renderer.
//              (Only present when HAS_CUDA_KERNEL is defined.)
//
// Assignment policy (calibrated dynamic queue):
//   Startup benchmark cost curves estimate aggregate CPU/GPU throughput for
//   this workload. Their ratio sizes the GPU's atomic queue claims while CPU
//   workers claim one tile at a time. If no matching profile exists, a bounded
//   hardware-class fallback batch is used. The shared queue still adapts to
//   spatially uneven escape work without a fixed CPU/GPU partition.
//
// The full viewport render completes once all tiles are consumed and all
// workers have joined. Results are written directly into a pre-allocated
// output cv::Mat.
//
// For the HTTP route, the scheduler is called synchronously (the HTTP handler
// waits for completion). Streaming preview (Phase 4) would push partial
// results via a callback — the interface is intentionally callback-ready.

#pragma once

#include "map_kernel.hpp"

#include <cstddef>
#include <functional>
#include <string>

namespace fsd::compute {

struct TileSchedulerStats {
    double total_ms   = 0.0;
    double cpu_ms     = 0.0;
    double gpu_ms     = 0.0;
    int    cpu_tiles  = 0;
    int    gpu_tiles  = 0;
    std::string scalar_used;
    std::string engine_used;
};

// Optional per-tile completion callback (for streaming preview).
// Arguments: (tile_x, tile_y, tile_w, tile_h, BGR rows in out).
using TileCallback = std::function<void(int tx, int ty, int tw, int th)>;

// Render the full viewport described by `p` using a hybrid tile scheduler.
// The output mat `out` must be pre-allocated to p.width × p.height × CV_8UC3.
// `tile_size` controls CPU subdivision granularity. GPU workers consume small
// tiles in dynamic batches, so no fixed CPU/GPU throughput split is assumed.
TileSchedulerStats render_map_hybrid(
    const MapParams& p,
    cv::Mat& out,
    int tile_size = 96,
    TileCallback on_tile_done = nullptr
);

// Field-output hybrid: each tile produces raw numeric data (FieldOutput)
// instead of BGR pixels. The tile results are scattered into a pre-allocated
// FieldOutput. Only Escape metric is supported (iter_u32 + norm_f32);
// non-escape metrics fall back to the OpenMP scalar field path.
TileSchedulerStats render_map_field_hybrid(
    const MapParams& p, FieldOutput& fo,
    int tile_size = 96);

} // namespace fsd::compute
