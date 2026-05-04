// compute/map_kernel.hpp
//
// Public entry for rendering a 2D map (Mandelbrot-family variant or Julia
// subset) into an OpenCV BGR Mat. All combinations of variant × metric are
// dispatched here. Transition (3D-rotated) slices live in transition_kernel.

#pragma once

#include "colormap.hpp"
#include "escape_time.hpp"
#include "variants.hpp"

#include <opencv2/core.hpp>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace fsd::compute {

// Function pointer type for user-compiled custom iteration step.
// Signature: step_fn(zr, zi, cr, ci, &zr_out, &zi_out)
using CustomStepFn = void(*)(double, double, double, double, double*, double*);

struct MapParams {
    // Center in parameter space and axis-aligned scale:
    //   - `scale` is the HEIGHT of the viewport in complex units.
    //   - width span is scale * width/height.
    double center_re = -0.75;
    double center_im =  0.0;
    double scale     =  3.0;

    int width        = 1024;
    int height       = 768;
    int iterations   = 1024;
    double bailout   = 2.0;  // radius, kept for UI-scale normalization
    double bailout_sq = 4.0; // squared threshold used by escape tests

    Variant  variant  = Variant::Mandelbrot;
    Metric   metric   = Metric::Escape;
    Colormap colormap = Colormap::ClassicCos;
    bool     smooth   = false;   // ln-smooth continuous coloring (requires norm)

    // Julia mode: if true, seed z = pixel point, c = (julia_re, julia_im).
    bool julia        = false;
    double julia_re   = 0.0;
    double julia_im   = 0.0;

    // Cap for the pairwise-distance orbit buffer (HS-Recurrence).
    int pairwise_cap  = 64;

    // CPU render threads. 0 means auto-select from visible logical cores;
    // callers with outer parallelism can force 1 to avoid nested oversubscribe.
    int render_threads = 0;

    // Scalar type: "fp32", "fp64", "fx64"/"q6.57", "q4.59",
    // experimental "q3.60", or "auto" (fp32/fp64/fx64 by viewport depth).
    std::string scalar_type = "auto";

    // Compute engine: "openmp" (default), "avx2", "avx512",
    // "cuda" (GPU kernel), or "hybrid". Silently falls back to openmp if
    // requested engine is unavailable.
    std::string engine = "openmp";

    // Custom variant: only set when variant == Variant::Custom.
    // Points to the step_fn symbol from a dlopen'd shared library.
    // Always uses OpenMP (no CUDA/AVX512 for custom formulas).
    CustomStepFn custom_step_fn = nullptr;

    // Optional cooperative cancellation hook for interactive renders. Kernels
    // check it at row/tile boundaries so stale viewport requests can exit
    // before finishing a full frame.
    std::function<bool()> should_cancel;
};

inline bool map_scalar_type_is_fp32(const std::string& scalar) noexcept {
    return scalar == "fp32" || scalar == "float32" || scalar == "float";
}

inline bool map_scalar_type_is_fp64(const std::string& scalar) noexcept {
    return scalar == "fp64" || scalar == "double" || scalar == "float64";
}

inline bool map_scalar_type_is_auto(const std::string& scalar) noexcept {
    return scalar.empty() || scalar == "auto";
}

inline double map_pixel_step(const MapParams& p) noexcept {
    if (!(p.scale > 0.0) || p.height <= 0 || !std::isfinite(p.scale)) return 0.0;
    return p.scale / static_cast<double>(p.height);
}

inline bool map_auto_fp32_is_adequate(const MapParams& p) noexcept {
    const double step = map_pixel_step(p);
    if (!(step > 0.0)) return false;

    double coord_mag = 1.0;
    coord_mag = std::max(coord_mag, std::fabs(p.center_re));
    coord_mag = std::max(coord_mag, std::fabs(p.center_im));
    if (p.julia) {
        coord_mag = std::max(coord_mag, std::fabs(p.julia_re));
        coord_mag = std::max(coord_mag, std::fabs(p.julia_im));
    }

    // Keep at least ~2 fp32 ulps per pixel step, so neighbouring pixels still
    // map to distinct coordinates before the renderer promotes to fp64.
    return step >= 2.0 * std::ldexp(coord_mag, -23);
}

inline std::string map_effective_scalar_type(const MapParams& p) {
    if (map_scalar_type_is_fp32(p.scalar_type)) return "fp32";
    if (map_scalar_type_is_fp64(p.scalar_type)) return "fp64";
    if (!map_scalar_type_is_auto(p.scalar_type)) return p.scalar_type;
    if (p.scale < 1e-13) return "fx64";
    return map_auto_fp32_is_adequate(p) ? "fp32" : "fp64";
}

struct MapStats {
    double elapsed_ms   = 0.0;
    int    pixel_count  = 0;
    std::string scalar_used;  // "fp32", "fp64", or "fx64"
    std::string engine_used;  // "openmp", "avx512", etc.
};

// Render a map into `out` (allocated BGR CV_8UC3 of size height x width).
// Returns stats including which scalar type and engine were actually used.
MapStats render_map(const MapParams& p, cv::Mat& out);

inline bool map_render_cancel_requested(const MapParams& p) {
    return p.should_cancel && p.should_cancel();
}

// Raw field output — no colorization.
// Escape metric  → iter_u32[W*H] (uint32 iter count) + norm_f32[W*H] (float32 |z|² at escape, 0 if bounded).
// Non-escape     → field_f64[W*H] (float64 raw metric value) + field_min/field_max.
// Always uses the OpenMP path (CUDA/AVX paths output BGR and can't easily expose raw data).
struct FieldOutput {
    int    width  = 0;
    int    height = 0;
    Metric metric = Metric::Escape;
    // Escape metric arrays (only populated when metric == Escape):
    std::vector<uint32_t> iter_u32;   // [W*H]
    std::vector<float>    norm_f32;   // [W*H], |z|² at escape; 0.0 if bounded
    // Non-escape metric array (only populated when metric != Escape):
    std::vector<double>   field_f64;  // [W*H]
    double field_min = 0.0;
    double field_max = 1.0;
    double elapsed_ms  = 0.0;
    std::string scalar_used;
    std::string engine_used = "openmp";
};

MapStats render_map_field(const MapParams& p, FieldOutput& out);

} // namespace fsd::compute
