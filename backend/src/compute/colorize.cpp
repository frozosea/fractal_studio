// compute/colorize.cpp
//
// Unified colorization: FieldOutput → BGR cv::Mat.

#include "colorize.hpp"
#include "color_program.hpp"
#include "colormap.hpp"
#include "ln_map.hpp"
#include "parallel.hpp"

#ifdef _OPENMP
#  include <omp.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fsd::compute {

namespace {

inline double normalize_raw_field(double raw, double bailout) {
    if (!std::isfinite(raw)) return 1.0;
    if (raw <= 0.0) return 0.0;
    return std::min(1.0, raw / bailout);
}

} // namespace

cv::Mat colorize_direct(const MapParams& p, const FieldOutput& field) {
    const int W = field.width;
    const int H = field.height;
    cv::Mat out(H, W, CV_8UC3);

    const int thread_count = resolve_render_threads(p.render_threads);

    if (field.metric == Metric::Escape) {
        const bool smooth = p.smooth
            && !field.norm_f32.empty()
            && field.norm_f32.size() == field.iter_u32.size();

        #pragma omp parallel for num_threads(thread_count) schedule(static)
        for (int y = 0; y < H; ++y) {
            uint8_t* row = out.ptr<uint8_t>(y);
            const size_t row_off = static_cast<size_t>(y) * static_cast<size_t>(W);
            for (int x = 0; x < W; ++x) {
                const size_t idx = row_off + static_cast<size_t>(x);
                const int iter = static_cast<int>(field.iter_u32[idx]);
                const double norm = smooth ? static_cast<double>(field.norm_f32[idx]) : 0.0;
                uint8_t* px = row + 3 * x;
                if (p.color_program) {
                    if (iter >= p.iterations) {
                        p.color_program->colorizeInterior(px[0], px[1], px[2]);
                    } else {
                        const double input = smooth
                            ? smooth_mu(iter, norm) / 32.0
                            : (static_cast<double>(iter) + 1.0) /
                                (static_cast<double>(p.iterations) + 2.0);
                        p.color_program->colorize(input, px[0], px[1], px[2]);
                    }
                } else {
                    colorize_escape_bgr(iter, p.iterations, p.colormap, norm, smooth,
                                        px[0], px[1], px[2]);
                }
            }
        }
    } else {
        #pragma omp parallel for num_threads(thread_count) schedule(static)
        for (int y = 0; y < H; ++y) {
            uint8_t* row = out.ptr<uint8_t>(y);
            const size_t row_off = static_cast<size_t>(y) * static_cast<size_t>(W);
            for (int x = 0; x < W; ++x) {
                const size_t idx = row_off + static_cast<size_t>(x);
                const double fv = field.field_f64[idx];
                uint8_t* px = row + 3 * x;
                if (p.color_program) {
                    if (!std::isfinite(fv)) p.color_program->colorizeInvalid(px[0], px[1], px[2]);
                    else p.color_program->colorize(
                        normalize_raw_field(fv, p.bailout), px[0], px[1], px[2]);
                } else if (p.colormap == Colormap::HsRainbow) {
                    colorize_field_hs_bgr(fv, px[0], px[1], px[2]);
                } else if (p.smooth) {
                    colorize_field_smooth_bgr(fv, p.colormap, px[0], px[1], px[2]);
                } else {
                    const double v01 = normalize_raw_field(fv, p.bailout);
                    colorize_field_bgr(v01, p.colormap, px[0], px[1], px[2]);
                }
            }
        }
    }
    return out;
}

cv::Mat colorize_equalized(const MapParams& p, const FieldOutput& field,
                           bool distance_weighted, double cycles_per_octave) {
    const auto eq = build_map_equalization(p, field, distance_weighted, cycles_per_octave);
    cv::Mat out;
    colorize_map_field_equalized(p, field, eq, out);
    return out;
}

cv::Mat colorize_field(const MapParams& p, const FieldOutput& field,
                       const std::string& color_mode, double cycles_per_octave) {
    if (color_mode == "eq_full" && field.metric == Metric::Escape) {
        return colorize_equalized(p, field, false, cycles_per_octave);
    }
    if (color_mode == "eq_center" && field.metric == Metric::Escape) {
        return colorize_equalized(p, field, true, cycles_per_octave);
    }
    return colorize_direct(p, field);
}

} // namespace fsd::compute
