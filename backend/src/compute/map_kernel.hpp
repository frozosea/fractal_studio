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
#include <numeric>
#include <string>
#include <vector>

namespace fsd::compute {

// Function pointer type for user-compiled custom iteration step.
// Signature: step_fn(zr, zi, cr, ci, &zr_out, &zi_out)
using CustomStepFn = void(*)(double, double, double, double, double*, double*);

// Called after a rectangular region of a raw FieldOutput has been fully
// written.  Interactive sessions use this to publish immutable patches while
// the same native-resolution render continues in the background.
using FieldTileCallback = std::function<void(int x, int y, int width, int height)>;

struct MapParams {
    // Center in parameter space and logical viewport:
    //   - `scale` is the HEIGHT of the viewport in complex units.
    //   - width span is scale * viewport_aspect.
    // `viewport_aspect <= 0` keeps the legacy width/height fallback.  Render
    // resolution controls sampling density only; it must not change the
    // logical complex-plane bounds.
    double center_re = -0.75;
    double center_im =  0.0;
    double scale     =  3.0;
    double viewport_aspect = 0.0;

    // Arbitrary-precision center (decimal string). When non-empty, the
    // perturbation reference orbit uses these instead of center_re/im.
    std::string center_re_str;
    std::string center_im_str;

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

    // Scalar type: "fp32", "fp64", "fp80"/"long_double",
    // "fp128"/"__float128", "fx64"/"q6.57", "q4.59",
    // experimental "q3.60", or "auto" (fp32/fp64/fx64 by viewport depth).
    std::string scalar_type = "auto";

    // Viewport rotation around center, in degrees.
    double rotation_deg = 0.0;

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

    // Optional publication hook for incremental raw-field consumers. It is
    // invoked only after the indicated output rectangle is complete; callers
    // must keep the FieldOutput storage alive until the render returns.
    FieldTileCallback on_field_tile_done;
};

inline double map_viewport_aspect(const MapParams& p) noexcept {
    if (std::isfinite(p.viewport_aspect) && p.viewport_aspect > 0.0) {
        return p.viewport_aspect;
    }
    return p.width > 0 && p.height > 0
        ? static_cast<double>(p.width) / static_cast<double>(p.height)
        : 1.0;
}

inline bool map_scalar_type_is_fp32(const std::string& scalar) noexcept {
    return scalar == "fp32" || scalar == "float32" || scalar == "float";
}

inline bool map_scalar_type_is_fp64(const std::string& scalar) noexcept {
    return scalar == "fp64" || scalar == "double" || scalar == "float64";
}

inline bool map_scalar_type_is_fp80(const std::string& scalar) noexcept {
    return scalar == "fp80" || scalar == "long_double" || scalar == "longdouble" ||
           scalar == "ldouble";
}

inline bool map_scalar_type_is_fp128(const std::string& scalar) noexcept {
    return scalar == "fp128" || scalar == "float128" || scalar == "__float128" ||
           scalar == "quad" || scalar == "binary128";
}

inline bool map_scalar_type_is_auto(const std::string& scalar) noexcept {
    return scalar.empty() || scalar == "auto";
}

// ---------------------------------------------------------------------------
// Perturbation precision combos.
//
// scalar_type can pin both halves of the perturbation renderer:
//   "perturbation"            auto reference tier, fp64 deltas (legacy alias)
//   "perturb-<ref>-<delta>"   ref   ∈ { fp64, fp128, mpfr, auto }
//                             delta ∈ { fp32, fp64, fp128 }
// e.g. "perturb-fp128-fp32" = __float128 reference orbit, fp32 pixel deltas.
// Underscores are accepted in place of dashes.
// ---------------------------------------------------------------------------

enum class PerturbRefPrec   { Auto, Fp64, Fp128, Mpfr };
enum class PerturbDeltaPrec { Fp32, Fp64, Fp128 };

struct PerturbMode {
    bool             requested = false;   // scalar_type names perturbation explicitly
    PerturbRefPrec   ref       = PerturbRefPrec::Auto;
    PerturbDeltaPrec delta     = PerturbDeltaPrec::Fp64;
};

inline PerturbMode map_scalar_perturb_mode(const std::string& scalar) noexcept {
    PerturbMode mode;
    if (scalar == "perturbation" || scalar == "perturb") {
        mode.requested = true;
        return mode;
    }
    std::string s = scalar;
    std::replace(s.begin(), s.end(), '_', '-');
    if (s.rfind("perturb-", 0) != 0) return mode;
    const std::string body = s.substr(8);
    const auto dash = body.find('-');
    if (dash == std::string::npos) return mode;
    const std::string ref = body.substr(0, dash);
    const std::string dlt = body.substr(dash + 1);
    if      (ref == "auto")  mode.ref = PerturbRefPrec::Auto;
    else if (ref == "fp64")  mode.ref = PerturbRefPrec::Fp64;
    else if (ref == "fp128") mode.ref = PerturbRefPrec::Fp128;
    else if (ref == "mpfr")  mode.ref = PerturbRefPrec::Mpfr;
    else return mode;
    if      (dlt == "fp32")  mode.delta = PerturbDeltaPrec::Fp32;
    else if (dlt == "fp64")  mode.delta = PerturbDeltaPrec::Fp64;
    else if (dlt == "fp128") mode.delta = PerturbDeltaPrec::Fp128;
    else return mode;
    mode.requested = true;
    return mode;
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
    if (map_scalar_type_is_fp80(p.scalar_type)) return "fp80";
    if (map_scalar_type_is_fp128(p.scalar_type)) return "fp128";
    // Perturbation combos are handled by the perturbation renderer; when a
    // request carrying one lands on a non-perturbation path (other variant or
    // metric), fall through to the auto ladder instead of leaking the token.
    if (!map_scalar_type_is_auto(p.scalar_type) &&
        !map_scalar_perturb_mode(p.scalar_type).requested) return p.scalar_type;

    // Auto precision ladder by zoom depth.
    // Perturbation (handled separately in render_map) covers Mandelbrot+Escape
    // at scale < 1e-7 with effectively unlimited precision.
    // This ladder handles non-perturbation cases (other variants/metrics).
    //
    // Thresholds based on ULP analysis (pixel_step = scale/H > ~10 × ULP).
    // ULP at coordinate magnitude ~1:
    //   fp64 ≈ 2^-52, fx64 ≈ 2^-57, fp80 ≈ 2^-63, fp128 ≈ 2^-112
    if (map_auto_fp32_is_adequate(p)) return "fp32";
    if (p.scale >= 1e-13) return "fp64";
    if (p.scale >= 5e-14) return "fx64";
    if (p.scale >= 1e-15) return "fp80";
#if defined(FSD_HAS_FLOAT128)
    return "fp128";
#else
    return "fp80";
#endif
}

struct MapStats {
    double elapsed_ms   = 0.0;
    int    pixel_count  = 0;
    std::string scalar_used;  // "fp32", "fp64", "fp80", "fp128", "fx64", etc.
    std::string engine_used;  // "openmp", "avx512", etc.
};

// Render a map into `out` (allocated BGR CV_8UC3 of size height x width).
// Returns stats including which scalar type and engine were actually used.
MapStats render_map(const MapParams& p, cv::Mat& out);

inline bool map_render_cancel_requested(const MapParams& p) {
    return p.should_cancel && p.should_cancel();
}

// Order independent raw-field work so its earliest completed tiles cover the
// whole viewport instead of only its top-left corner.  This changes scheduling
// order, not coordinates or the number of native pixels computed.  Reversing
// coordinate bits before Morton interleaving yields a coarse-to-fine lattice:
// e.g. the first 4x4 tile positions visit the quadrant anchors first.
inline uint32_t map_progressive_reverse_bits(uint32_t value, unsigned bits) noexcept {
    uint32_t reversed = 0;
    for (unsigned bit = 0; bit < bits; ++bit) {
        reversed = (reversed << 1u) | ((value >> bit) & 1u);
    }
    return reversed;
}

inline uint64_t map_progressive_work_rank(uint32_t x, uint32_t y, unsigned bits) noexcept {
    const uint32_t rx = map_progressive_reverse_bits(x, bits);
    const uint32_t ry = map_progressive_reverse_bits(y, bits);
    uint64_t rank = 0;
    for (unsigned bit = 0; bit < bits; ++bit) {
        rank |= static_cast<uint64_t>((rx >> bit) & 1u) << (2u * bit);
        rank |= static_cast<uint64_t>((ry >> bit) & 1u) << (2u * bit + 1u);
    }
    return rank;
}

inline std::vector<int> map_field_progressive_order(int columns, int rows, bool progressive) {
    if (columns <= 0 || rows <= 0) return {};
    const size_t count = static_cast<size_t>(columns) * static_cast<size_t>(rows);
    std::vector<int> order(count);
    if (!progressive) {
        std::iota(order.begin(), order.end(), 0);
        return order;
    }

    const unsigned extent = static_cast<unsigned>(std::max(columns, rows));
    unsigned bits = 0;
    while ((1u << bits) < extent) ++bits;

    std::vector<std::pair<uint64_t, int>> ranked;
    ranked.reserve(count);
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < columns; ++x) {
            ranked.emplace_back(
                map_progressive_work_rank(static_cast<uint32_t>(x), static_cast<uint32_t>(y), bits),
                y * columns + x);
        }
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    for (size_t i = 0; i < ranked.size(); ++i) order[i] = ranked[i].second;
    return order;
}

// Raw field output — no colorization.
// Escape metric  → iter_u32[W*H] (uint32 iter count) + norm_f32[W*H] (float32 |z|² at escape, 0 if bounded).
// Non-escape     → field_f64[W*H] (float64 raw metric value) + field_min/field_max.
// Uses a CPU scalar/SIMD field path when available. Progressive consumers may
// request tile publication; whole-frame CUDA/hybrid plans then fall back to a
// CPU path so the same FieldOutput can be published while it is computed.
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
