// compute/ln_map.hpp
//
// Shared ln-map strip renderer used by /api/map/ln and /api/video/export.

#pragma once

#include "colormap.hpp"
#include "variants.hpp"

#include <opencv2/core.hpp>

#include <functional>
#include <string>

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
};

struct LnMapStats {
    double elapsed_ms = 0.0;
    int pixel_count = 0;
    std::string engine_used = "openmp";
    std::string scalar_used = "fp64";
    std::string precision_mode = "standard";
    std::string layer_summary;
    std::string validation_summary;
};

using LnMapProgress = std::function<void(int rowsDone)>;

bool ln_map_variant_supported_by_simd(Variant v);
bool ln_map_color_mode_supported(const std::string& mode) noexcept;
bool ln_map_avx2_available() noexcept;
LnMapStats render_ln_map_openmp(const LnMapParams& p, cv::Mat& out, const LnMapProgress& on_row_done = nullptr);
LnMapStats render_ln_map_openmp_rows(const LnMapParams& p, cv::Mat& out, int row_start, int row_count, const LnMapProgress& on_row_done = nullptr);
LnMapStats render_ln_map_avx512(const LnMapParams& p, cv::Mat& out, const LnMapProgress& on_row_done = nullptr);
LnMapStats render_ln_map_avx512_rows(const LnMapParams& p, cv::Mat& out, int row_start, int row_count, const LnMapProgress& on_row_done = nullptr);
LnMapStats render_ln_map_avx512_fp32_rows(const LnMapParams& p, cv::Mat& out, int row_start, int row_count, const LnMapProgress& on_row_done = nullptr);
LnMapStats render_ln_map_avx2(const LnMapParams& p, cv::Mat& out, const LnMapProgress& on_row_done = nullptr);
LnMapStats render_ln_map_avx2_rows(const LnMapParams& p, cv::Mat& out, int row_start, int row_count, const LnMapProgress& on_row_done = nullptr);
LnMapStats render_ln_map_avx2_fp32_rows(const LnMapParams& p, cv::Mat& out, int row_start, int row_count, const LnMapProgress& on_row_done = nullptr);
LnMapStats render_ln_map(const LnMapParams& p, cv::Mat& out, const LnMapProgress& on_row_done = nullptr);

} // namespace fsd::compute
