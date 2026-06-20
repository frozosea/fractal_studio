// compute/ln_map.hpp
//
// Shared ln-map strip renderer used by /api/map/ln and /api/video/export.

#pragma once

#include "colormap.hpp"
#include "map_kernel.hpp"   // MapParams, FieldOutput (cartesian equalized coloring)
#include "variants.hpp"

#include <opencv2/core.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fsd::compute {

struct LnMapParams {
    bool julia = false;
    double center_re = 0.0;
    double center_im = 0.0;
    double julia_re = 0.0;
    double julia_im = 0.0;
    int width_s = 1024;
    int height_t = 4096;
    int iterations = 2048;
    double bailout = 2.0;
    double bailout_sq = 4.0;
    Variant variant = Variant::Mandelbrot;
    Colormap colormap = Colormap::ClassicCos;
    std::string color_mode = "escape"; // escape, hist_eq, row_eq, log_lift, bands, frontier
    std::string engine = "auto"; // auto, hybrid, cuda, avx512, avx2, openmp
    std::string precision_mode = "standard"; // standard, fast
    std::string scalar_type = "auto"; // auto, fp64, fx64
    double fast_fp32_depth_octaves = 18.0;
    double fast_fp64_depth_octaves = 34.0;
    bool fast_validate = true;
    double fast_validation_band_octaves = 4.0;
    int fast_validation_sample_rows = 5;
    int fast_validation_sample_cols = 24;
    double fast_validation_max_mismatch_ratio = 0.01;
    int fast_validation_max_p99_iter_delta = 16;
    double fast_validation_max_mean_color_delta = 8.0;
    // Periodic (hist_eq) coloring: number of full palette cycles per zoom octave.
    // Total cycles down the strip = total_octaves * color_cycles_per_octave, i.e.
    // ≈ log2(zoom magnification) cycles when this is 1.0. Default 0.5 (broader bands).
    double color_cycles_per_octave = 0.5;
};

// Shared periodic coloring for the hist_eq ln-map color mode. Built once from the
// ln-map strip and reused by the final cartesian frame so any pixel with a given
// escape count gets the SAME color in both → seamless warp blend.
//
// Mapping is direct and discrete: phase = (count - count_min) / period, where the
// period is chosen so the whole strip spans total_octaves*cyclesPerOctave palette
// cycles (= log2(magnification) cycles when cyclesPerOctave is 1). One escape count =
// one solid color band (no smoothing). The shallowest counts (the zoom opening) fall
// in the first 1/6 cycle, which fades pure black → the palette start color (green for
// spectral1530); after that the palette cycles.
struct LnMapEqualization {
    double   count_min      = 0.0;         // globally shallowest escape count → phase 0
    double   period         = 1.0;         // escape counts per palette cycle
    double   onset_cycles   = 1.0 / 6.0;   // length of the black→start-color lead-in, in cycles
    bool     colormap_wraps = false;       // frac (true) vs triangle reflect (false)
    Colormap colormap       = Colormap::ClassicCos;
    bool     valid          = false;
    // Periodic color for an escaped pixel with escape count `it`. Interior pixels are
    // handled by the caller (kept white); do not call this for it >= iterations.
    void colorize(int it, uint8_t& b, uint8_t& g, uint8_t& r) const;
};

struct LnMapStats {
    double elapsed_ms = 0.0;
    int pixel_count = 0;
    std::string engine_used = "openmp";
    std::string scalar_used = "fp64";
    std::string precision_mode = "standard";
    std::string layer_summary;
    std::string validation_summary;
    LnMapEqualization equalization;        // populated only for color_mode == "hist_eq"
};

LnMapEqualization reconstructEqualization(
    double count_min, double period, double onset_cycles,
    bool colormap_wraps, Colormap colormap);

// Build a periodic equalization from a *cartesian* escape-count field (FieldOutput from
// render_map_field), so the live map viewer can preview the ln-map's equalized coloring.
// distance_weighted=false → full-image (every escaped pixel, unit weight); true → center
// weighted (1/ρ² toward the view center, restricted to the |c|<=2 disk — faithful to the
// ln-map's log-polar measure). The period tracks zoom depth (~1 cycle/octave × cpo).
// Requires field.metric == Escape (iter_u32 populated).
LnMapEqualization build_map_equalization(
    const MapParams& p, const FieldOutput& field, bool distance_weighted, double cpo);

// Colorize a cartesian escape-count field through a periodic equalization: interior
// pixels (it >= iterations) → white, escaped pixels → eq.colorize(it). Shared by the live
// map viewer and the video export's deferred final-frame coloring.
void colorize_map_field_equalized(
    const MapParams& p, const FieldOutput& field,
    const LnMapEqualization& eq, cv::Mat& out);

using LnMapProgress = std::function<void(int rowsDone)>;

bool ln_map_variant_supported_by_simd(Variant v);
bool ln_map_color_mode_supported(const std::string& mode) noexcept;
bool ln_map_avx2_available() noexcept;
LnMapStats render_ln_map_openmp(const LnMapParams& p, cv::Mat& out, const LnMapProgress& on_row_done = nullptr);
LnMapStats render_ln_map_openmp_rows(const LnMapParams& p, cv::Mat& out, int row_start, int row_count, const LnMapProgress& on_row_done = nullptr);
LnMapStats render_ln_map_avx512(const LnMapParams& p, cv::Mat& out, const LnMapProgress& on_row_done = nullptr);
LnMapStats render_ln_map_avx512_rows(const LnMapParams& p, cv::Mat& out, int row_start, int row_count, const LnMapProgress& on_row_done = nullptr);
LnMapStats render_ln_map_avx512_fp32_rows(const LnMapParams& p, cv::Mat& out, int row_start, int row_count, const LnMapProgress& on_row_done = nullptr);
// Raw escape-count field output (fp64, exact) for the mapped color modes, so hist_eq can
// use SIMD for the iteration pass instead of degrading to scalar OpenMP. Return false if
// the path is unavailable (caller falls back to OpenMP).
bool render_ln_map_avx512_iters_rows(const LnMapParams& p, int* iters, int row_start, int row_count, const LnMapProgress& on_row_done = nullptr);
bool render_ln_map_avx512_fp32_iters_rows(const LnMapParams& p, int* iters, int row_start, int row_count, const LnMapProgress& on_row_done = nullptr);
bool render_ln_map_avx2_iters_rows(const LnMapParams& p, int* iters, int row_start, int row_count, const LnMapProgress& on_row_done = nullptr);
bool render_ln_map_avx2_fp32_iters_rows(const LnMapParams& p, int* iters, int row_start, int row_count, const LnMapProgress& on_row_done = nullptr);
LnMapStats render_ln_map_avx2(const LnMapParams& p, cv::Mat& out, const LnMapProgress& on_row_done = nullptr);
LnMapStats render_ln_map_avx2_rows(const LnMapParams& p, cv::Mat& out, int row_start, int row_count, const LnMapProgress& on_row_done = nullptr);
LnMapStats render_ln_map_avx2_fp32_rows(const LnMapParams& p, cv::Mat& out, int row_start, int row_count, const LnMapProgress& on_row_done = nullptr);
LnMapStats render_ln_map(const LnMapParams& p, cv::Mat& out, const LnMapProgress& on_row_done = nullptr);

} // namespace fsd::compute
