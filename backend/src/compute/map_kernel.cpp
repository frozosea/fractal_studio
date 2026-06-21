// compute/map_kernel.cpp
//
// OpenMP-parallel map renderer. For each pixel, iterates the chosen variant
// under the chosen metric and writes a BGR byte triple into the output Mat.
//
// Supports scalar types:
//   fp32  — std::float (fast shallow-depth interactive previews)
//   fp64  — std::double (default, good to ~1e-13 zoom depth)
//   fp80  — long double (OpenMP scalar path)
//   fp128 — __float128/libquadmath when available (OpenMP scalar path)
//   fx64  — Fx64 fixed-point 1s·6i·57f (good to ~1e-17, ~4 extra magnitudes)
//
// The variant is dispatched at compile time via `variant_step<V,S>`.

#include "map_kernel.hpp"
#include "map_kernel_avx2.hpp"
#include "map_kernel_avx512.hpp"
#include "engine_select.hpp"
#include "escape_time.hpp"
#include "complex.hpp"
#include "fx64_raw.hpp"
#include "parallel.hpp"
#include "scalar/fx64.hpp"
#include "tile_scheduler.hpp"

#if defined(HAS_CUDA_KERNEL)
#  include "cuda/map_kernel.cuh"
#  define USE_CUDA 1
#else
#  define USE_CUDA 0
#endif

#ifdef _OPENMP
#  include <omp.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace fsd::compute {

namespace {

inline constexpr IterResultMask NeedEscape =
    IterResultField::Iter | IterResultField::Escaped;
inline constexpr IterResultMask NeedEscapeSmooth =
    NeedEscape | IterResultField::Norm;
inline constexpr IterResultMask NeedMinAbs = IterResultField::MinAbs;
inline constexpr IterResultMask NeedMaxAbs = IterResultField::MaxAbs;
inline constexpr IterResultMask NeedEnvelope =
    IterResultField::MinAbs | IterResultField::MaxAbs;

template <Metric M>
inline double raw_field_value(const IterResult& r) {
    if constexpr (M == Metric::MinAbs) {
        return std::isfinite(r.min_abs) ? r.min_abs : 0.0;
    } else if constexpr (M == Metric::MaxAbs) {
        return r.max_abs > 0.0 ? r.max_abs : 0.0;
    } else if constexpr (M == Metric::Envelope) {
        return std::isfinite(r.min_abs) ? 0.5 * (r.min_abs + r.max_abs) : 0.0;
    } else if constexpr (M == Metric::MinPairwiseDist) {
        return std::isfinite(r.extra) ? r.extra : 0.0;
    } else {
        return 0.0;
    }
}

template <Metric M>
inline double normalize_field_static(const IterResult& r, double bailout) {
    if constexpr (M == Metric::MinAbs) {
        if (!std::isfinite(r.min_abs)) return 1.0;
        return std::min(1.0, r.min_abs / bailout);
    } else if constexpr (M == Metric::MaxAbs) {
        if (r.max_abs <= 0.0) return 0.0;
        return std::min(1.0, r.max_abs / bailout);
    } else if constexpr (M == Metric::Envelope) {
        if (!std::isfinite(r.min_abs)) return 1.0;
        return std::min(1.0, 0.5 * (r.min_abs + r.max_abs) / bailout);
    } else if constexpr (M == Metric::MinPairwiseDist) {
        if (!std::isfinite(r.extra)) return 1.0;
        return std::min(1.0, r.extra / bailout);
    } else {
        return 0.0;
    }
}

inline int ceil_div(int value, int step) {
    return (value + step - 1) / step;
}

[[noreturn]] void throw_render_cancelled() {
    throw std::runtime_error("cancelled");
}

inline bool mark_cancelled_if_requested(const MapParams& p, std::atomic<bool>& cancelled) {
    if (cancelled.load(std::memory_order_relaxed)) return true;
    if (map_render_cancel_requested(p)) {
        cancelled.store(true, std::memory_order_relaxed);
        return true;
    }
    return false;
}

inline void throw_if_cancelled(const MapParams& p, const std::atomic<bool>& cancelled) {
    if (cancelled.load(std::memory_order_relaxed) || map_render_cancel_requested(p)) {
        throw_render_cancelled();
    }
}

// Generic OpenMP kernel templated on Variant, Scalar, and metric.
template <Variant V, typename S, Metric M, IterResultMask NeedMask>
void render_variant_metric_impl(const MapParams& p, cv::Mat& out) {
    const int W = p.width;
    const int H = p.height;
    const double aspect  = static_cast<double>(W) / static_cast<double>(H);
    const double span_im = p.scale;
    const double span_re = p.scale * aspect;
    const double re_min  = p.center_re - span_re * 0.5;
    const double im_max  = p.center_im + span_im * 0.5;
    const double bail2   = p.bailout_sq;
    const int thread_count = resolve_render_threads(p.render_threads);
    constexpr int tile_size = 32;
    const int tiles_x = ceil_div(W, tile_size);
    const int tiles_y = ceil_div(H, tile_size);
    const int tile_count = tiles_x * tiles_y;
    std::atomic<bool> cancelled{false};

    const S jre = scalar_from_double<S>(p.julia_re);
    const S jim = scalar_from_double<S>(p.julia_im);
    const Cx<S> c_const{jre, jim};

    #pragma omp parallel num_threads(thread_count)
    {
        std::vector<Cx<S>> orbit;
        if constexpr (M == Metric::MinPairwiseDist) {
            orbit.reserve(static_cast<size_t>(p.pairwise_cap));
        }

        #pragma omp for schedule(dynamic, 1)
        for (int tile = 0; tile < tile_count; tile++) {
            if (mark_cancelled_if_requested(p, cancelled)) continue;
            const int tile_x = tile % tiles_x;
            const int tile_y = tile / tiles_x;
            const int x0 = tile_x * tile_size;
            const int y0 = tile_y * tile_size;
            const int x1 = std::min(W, x0 + tile_size);
            const int y1 = std::min(H, y0 + tile_size);

            for (int y = y0; y < y1; y++) {
                if (mark_cancelled_if_requested(p, cancelled)) break;
                uint8_t* row = out.ptr<uint8_t>(y);
                const double im_d = im_max - (static_cast<double>(y) + 0.5) / H * span_im;
                const S im = scalar_from_double<S>(im_d);
                for (int x = x0; x < x1; x++) {
                    const double re_d = re_min + (static_cast<double>(x) + 0.5) / W * span_re;
                    const S re = scalar_from_double<S>(re_d);

                    Cx<S> z0;
                    Cx<S> c;
                    if (p.julia) {
                        z0 = Cx<S>{re, im};
                        c  = c_const;
                    } else {
                        z0 = Cx<S>{scalar_from_double<S>(0.0), scalar_from_double<S>(0.0)};
                        c  = Cx<S>{re, im};
                    }

                    IterResult r;
                    if constexpr (M == Metric::MinPairwiseDist) {
                        r = iterate_pairwise<V, S>(
                            z0, c, p.iterations, p.bailout, bail2, p.pairwise_cap, orbit);
                    } else {
                        r = iterate_masked<NeedMask, V, S>(
                            z0, c, p.iterations, p.bailout, bail2);
                    }

                    uint8_t* px = row + 3 * x;
                    if constexpr (M == Metric::Escape) {
                        const int iter = r.escaped ? r.iter : p.iterations;
                        constexpr bool smooth_escape =
                            iter_result_wants(NeedMask, IterResultField::Norm);
                        const double norm = smooth_escape && r.escaped ? r.norm : 0.0;
                        colorize_escape_bgr(iter, p.iterations, p.colormap, norm, smooth_escape, px[0], px[1], px[2]);
                    } else {
                        if (p.colormap == Colormap::HsRainbow) {
                            const double fv = raw_field_value<M>(r);
                            colorize_field_hs_bgr(fv, px[0], px[1], px[2]);
                        } else if (p.smooth) {
                            const double fv = raw_field_value<M>(r);
                            colorize_field_smooth_bgr(fv, p.colormap, px[0], px[1], px[2]);
                        } else {
                            const double v01 = normalize_field_static<M>(r, p.bailout);
                            colorize_field_bgr(v01, p.colormap, px[0], px[1], px[2]);
                        }
                    }
                }
            }
        }
    }
    throw_if_cancelled(p, cancelled);
}

template <int FRAC, Variant V, Metric M, IterResultMask NeedMask>
void render_variant_metric_fixed_impl(const MapParams& p, cv::Mat& out) {
    static_assert(!variant_is_transcendental_v<V>(),
        "Fixed-point integer renderer only supports quadratic variants.");
    static_assert(M != Metric::MinPairwiseDist,
        "Fixed-point integer renderer does not handle pairwise distance.");

    using S = Fixed64<FRAC>;
    const int W = p.width;
    const int H = p.height;
    const FixedViewportRaw<FRAC> vp = make_fixed_viewport_raw<FRAC>(
        p.center_re, p.center_im, p.scale, W, H,
        p.julia_re, p.julia_im, p.bailout, p.bailout_sq);
    const int thread_count = resolve_render_threads(p.render_threads);
    constexpr int tile_size = 32;
    const int tiles_x = ceil_div(W, tile_size);
    const int tiles_y = ceil_div(H, tile_size);
    const int tile_count = tiles_x * tiles_y;
    std::atomic<bool> cancelled{false};

    const Cx<S> c_const{S{vp.julia_re_raw}, S{vp.julia_im_raw}};

    #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 1)
    for (int tile = 0; tile < tile_count; tile++) {
        if (mark_cancelled_if_requested(p, cancelled)) continue;
        const int tile_x = tile % tiles_x;
        const int tile_y = tile / tiles_x;
        const int x0 = tile_x * tile_size;
        const int y0 = tile_y * tile_size;
        const int x1 = std::min(W, x0 + tile_size);
        const int y1 = std::min(H, y0 + tile_size);

        for (int y = y0; y < y1; y++) {
            if (mark_cancelled_if_requested(p, cancelled)) break;
            uint8_t* row = out.ptr<uint8_t>(y);
            const S im{fixed_pixel_im_raw<FRAC>(vp, y)};
            for (int x = x0; x < x1; x++) {
                const S re{fixed_pixel_re_raw<FRAC>(vp, x)};

                Cx<S> z0;
                Cx<S> c;
                if (p.julia) {
                    z0 = Cx<S>{re, im};
                    c  = c_const;
                } else {
                    z0 = Cx<S>{S{INT64_C(0)}, S{INT64_C(0)}};
                    c  = Cx<S>{re, im};
                }

                constexpr FixedEscapeGate gate = FRAC == 60
                    ? FixedEscapeGate::L1
                    : (FRAC == 59 ? FixedEscapeGate::Component : FixedEscapeGate::Direct);
                const IterResult r = iterate_fixed_int_masked<FRAC, gate, NeedMask, V>(
                    z0, c, p.iterations, vp.bailout_raw, vp.bailout2_raw,
                    vp.two_raw, vp.two_sqrt2_floor_raw, p.julia);

                uint8_t* px = row + 3 * x;
                if constexpr (M == Metric::Escape) {
                    const int iter = r.escaped ? r.iter : p.iterations;
                    colorize_escape_bgr(iter, p.iterations, p.colormap, 0.0, false, px[0], px[1], px[2]);
                } else {
                    if (p.colormap == Colormap::HsRainbow) {
                        const double fv = raw_field_value<M>(r);
                        colorize_field_hs_bgr(fv, px[0], px[1], px[2]);
                    } else if (p.smooth) {
                        const double fv = raw_field_value<M>(r);
                        colorize_field_smooth_bgr(fv, p.colormap, px[0], px[1], px[2]);
                    } else {
                        const double v01 = normalize_field_static<M>(r, p.bailout);
                        colorize_field_bgr(v01, p.colormap, px[0], px[1], px[2]);
                    }
                }
            }
        }
    }
    throw_if_cancelled(p, cancelled);
}

// Variant dispatch helpers — scalar OpenMP plus fixed-point integer paths.
template <Variant V, typename S>
void render_variant_scalar(const MapParams& p, cv::Mat& out) {
    switch (p.metric) {
        case Metric::Escape:
            if (p.smooth) render_variant_metric_impl<V, S, Metric::Escape, NeedEscapeSmooth>(p, out);
            else          render_variant_metric_impl<V, S, Metric::Escape, NeedEscape>(p, out);
            break;
        case Metric::MinAbs:
            render_variant_metric_impl<V, S, Metric::MinAbs, NeedMinAbs>(p, out); break;
        case Metric::MaxAbs:
            render_variant_metric_impl<V, S, Metric::MaxAbs, NeedMaxAbs>(p, out); break;
        case Metric::Envelope:
            render_variant_metric_impl<V, S, Metric::Envelope, NeedEnvelope>(p, out); break;
        case Metric::MinPairwiseDist:
            render_variant_metric_impl<V, S, Metric::MinPairwiseDist, IterResultField::Extra>(p, out); break;
    }
}

template <Variant V>
void render_variant_fp32(const MapParams& p, cv::Mat& out) {
    render_variant_scalar<V, float>(p, out);
}

template <Variant V>
void render_variant(const MapParams& p, cv::Mat& out) {
    render_variant_scalar<V, double>(p, out);
}

template <Variant V>
void render_variant_fp80(const MapParams& p, cv::Mat& out) {
    render_variant_scalar<V, long double>(p, out);
}

#if defined(FSD_HAS_FLOAT128)
template <Variant V>
void render_variant_fp128(const MapParams& p, cv::Mat& out) {
    render_variant_scalar<V, __float128>(p, out);
}
#endif

template <int FRAC, Variant V>
void render_variant_fixed(const MapParams& p, cv::Mat& out) {
    static_assert(!variant_is_transcendental_v<V>(),
        "Fixed-point integer renderer only supports quadratic variants.");
    switch (p.metric) {
        case Metric::Escape:
            render_variant_metric_fixed_impl<FRAC, V, Metric::Escape, NeedEscape>(p, out);
            break;
        case Metric::MinAbs:
            render_variant_metric_fixed_impl<FRAC, V, Metric::MinAbs, NeedMinAbs>(p, out); break;
        case Metric::MaxAbs:
            render_variant_metric_fixed_impl<FRAC, V, Metric::MaxAbs, NeedMaxAbs>(p, out); break;
        case Metric::Envelope:
            render_variant_metric_fixed_impl<FRAC, V, Metric::Envelope, NeedEnvelope>(p, out); break;
        case Metric::MinPairwiseDist:
            render_variant_metric_impl<V, double, Metric::MinPairwiseDist, IterResultField::Extra>(p, out); break;
    }
}

} // namespace

enum class FixedPrecision {
    None,
    Q657,
    Q459,
    Q360,
};

static FixedPrecision requested_fixed_precision(const MapParams& p) {
    const std::string scalar = map_effective_scalar_type(p);
    if (scalar == "q3.60" || scalar == "q360" ||
        scalar == "fx60" || scalar == "fixed60") {
        return FixedPrecision::Q360;
    }
    if (scalar == "q4.59" || scalar == "q459" ||
        scalar == "fx59" || scalar == "fixed59") {
        return FixedPrecision::Q459;
    }
    if (scalar == "fx64" || scalar == "q6.57" ||
        scalar == "q657" || scalar == "fixed57") {
        return FixedPrecision::Q657;
    }
    return FixedPrecision::None;
}

static const char* fixed_precision_name(FixedPrecision precision) {
    switch (precision) {
        case FixedPrecision::Q360: return "q3.60";
        case FixedPrecision::Q459: return "q4.59";
        case FixedPrecision::Q657: return "fx64";
        case FixedPrecision::None: return "fp64";
    }
    return "fp64";
}

static bool supports_fixed_int_path(const MapParams& p, bool field_output = false) {
    const int variant_id = static_cast<int>(p.variant);
    if (variant_id < 0 || variant_id > 9) return false;
    if (p.metric == Metric::MinPairwiseDist) return false;
    if (!field_output && p.smooth) return false;
    return true;
}

template <int FRAC>
static bool fixed_double_fits_raw(double value) {
    if (!std::isfinite(value)) return false;
    const long double scaled = static_cast<long double>(value) * std::ldexp(1.0L, FRAC);
    return scaled < static_cast<long double>(std::numeric_limits<int64_t>::max()) &&
           scaled > static_cast<long double>(std::numeric_limits<int64_t>::min());
}

template <int FRAC>
static bool fixed_double_fits_uraw(double value) {
    if (!std::isfinite(value) || value < 0.0) return false;
    const long double scaled = static_cast<long double>(value) * std::ldexp(1.0L, FRAC);
    return scaled < static_cast<long double>(std::numeric_limits<uint64_t>::max());
}

static bool i128_fits_i64(__int128 value) {
    return value <= static_cast<__int128>(std::numeric_limits<int64_t>::max()) &&
           value >= static_cast<__int128>(std::numeric_limits<int64_t>::min());
}

template <int FRAC>
static bool fixed_viewport_representable(const MapParams& p) {
    if (p.width <= 0 || p.height <= 0 || p.scale <= 0.0) return false;
    if (!fixed_double_fits_raw<FRAC>(p.center_re) ||
        !fixed_double_fits_raw<FRAC>(p.center_im) ||
        !fixed_double_fits_raw<FRAC>(p.scale) ||
        !fixed_double_fits_raw<FRAC>(p.julia_re) ||
        !fixed_double_fits_raw<FRAC>(p.julia_im) ||
        !fixed_double_fits_uraw<FRAC>(p.bailout) ||
        !fixed_double_fits_uraw<FRAC>(p.bailout_sq)) {
        return false;
    }

    const int64_t center_re_raw = fixed_round_to_raw_sat<FRAC>(p.center_re);
    const int64_t center_im_raw = fixed_round_to_raw_sat<FRAC>(p.center_im);
    const int64_t scale_raw = fixed_round_to_raw_sat<FRAC>(p.scale);
    const __int128 span_im_raw = static_cast<__int128>(scale_raw);
    const __int128 span_re_raw =
        (static_cast<__int128>(scale_raw) * p.width) / p.height;
    const __int128 step_re_raw = span_re_raw / p.width;
    const __int128 step_im_raw = span_im_raw / p.height;
    const __int128 first_re_raw =
        static_cast<__int128>(center_re_raw) - span_re_raw / 2 + step_re_raw / 2;
    const __int128 first_im_raw =
        static_cast<__int128>(center_im_raw) + span_im_raw / 2 - step_im_raw / 2;
    const __int128 last_re_raw =
        first_re_raw + static_cast<__int128>(p.width - 1) * step_re_raw;
    const __int128 last_im_raw =
        first_im_raw - static_cast<__int128>(p.height - 1) * step_im_raw;

    return i128_fits_i64(step_re_raw) &&
           i128_fits_i64(step_im_raw) &&
           i128_fits_i64(first_re_raw) &&
           i128_fits_i64(first_im_raw) &&
           i128_fits_i64(last_re_raw) &&
           i128_fits_i64(last_im_raw);
}

static bool fixed_precision_safe(const MapParams& p, FixedPrecision precision) {
    switch (precision) {
        case FixedPrecision::Q360: return fixed_viewport_representable<60>(p);
        case FixedPrecision::Q459: return fixed_viewport_representable<59>(p);
        case FixedPrecision::Q657: return fixed_viewport_representable<57>(p);
        case FixedPrecision::None:
            return false;
    }
    return false;
}

static FixedPrecision select_fixed_precision(const MapParams& p) {
    const FixedPrecision requested = requested_fixed_precision(p);
    if (requested == FixedPrecision::None) return FixedPrecision::None;
    if (fixed_precision_safe(p, requested)) return requested;
    return FixedPrecision::None;
}

// Resolved precision + engine for a map render. Extracted so render_map (BGR) and
// render_map_field (raw iter/norm) share one selection path. `field_output` flips the two
// caller-specific decisions: (1) fx ignores smooth for the field path (the field kernels
// produce norm), and (2) needs_norm is only meaningful for BGR smooth coloring — the field
// kernels always emit norm, so it never forces the field path back to scalar.
struct MapEnginePlan {
    std::string    engine;                          // "cuda"|"avx512"|"avx2"|"openmp"|"hybrid"
    bool           fp32 = false, fp80 = false, fp128 = false, fx = false;
    FixedPrecision fixed_precision = FixedPrecision::None;
    bool           needs_norm = false;
    bool           scalar_fallback = false;
};

static MapEnginePlan select_map_engine_plan(const MapParams& p, bool field_output) {
    MapEnginePlan plan;
    const std::string effective_scalar = map_effective_scalar_type(p);
    plan.fp32  = effective_scalar == "fp32";
    plan.fp80  = effective_scalar == "fp80";
    plan.fp128 = effective_scalar == "fp128";
    plan.fixed_precision = select_fixed_precision(p);
    plan.fx = plan.fixed_precision != FixedPrecision::None &&
              supports_fixed_int_path(p, field_output);

    std::string selected_engine = select_map_engine(p, plan.fx);
    if (plan.fp32 && p.engine == "auto") {
        if (map_engine_supported(p, "cuda", false)) {
            selected_engine = "cuda";
        } else if (avx512_available() && map_engine_supported(p, "avx512", false)) {
            selected_engine = "avx512";
        } else if (avx2_available() && fma_available() && map_engine_supported(p, "avx2", false)) {
            selected_engine = "avx2";
        } else {
            selected_engine = "openmp";
        }
    }
    if (plan.fp32 && selected_engine == "hybrid") {
        if (avx512_available() && map_engine_supported(p, "avx512", false)) {
            selected_engine = "avx512";
        } else if (avx2_available() && fma_available() && map_engine_supported(p, "avx2", false)) {
            selected_engine = "avx2";
        } else {
            selected_engine = "openmp";
        }
    }
    plan.engine = selected_engine;

    // smooth (BGR) needs per-pixel |z|² which the AVX-512/CUDA *colorize* paths don't track;
    // the field kernels always emit norm, so field renders never gate on smooth.
    plan.needs_norm = field_output ? false : p.smooth;
    // Trig variants need scalar std::cmath — skip AVX-512/AVX-2/CUDA for them.
    plan.scalar_fallback = variant_needs_scalar_fallback(p.variant);
    return plan;
}

// Dispatch fp32/fp64 variants.
static void dispatch_fp32(const MapParams& p, cv::Mat& out) {
    switch (p.variant) {
        case Variant::Mandelbrot: render_variant_fp32<Variant::Mandelbrot>(p, out); break;
        case Variant::Tri:        render_variant_fp32<Variant::Tri>(p, out);        break;
        case Variant::Boat:       render_variant_fp32<Variant::Boat>(p, out);       break;
        case Variant::Duck:       render_variant_fp32<Variant::Duck>(p, out);       break;
        case Variant::Bell:       render_variant_fp32<Variant::Bell>(p, out);       break;
        case Variant::Fish:       render_variant_fp32<Variant::Fish>(p, out);       break;
        case Variant::Vase:       render_variant_fp32<Variant::Vase>(p, out);       break;
        case Variant::Bird:       render_variant_fp32<Variant::Bird>(p, out);       break;
        case Variant::Mask:       render_variant_fp32<Variant::Mask>(p, out);       break;
        case Variant::Ship:       render_variant_fp32<Variant::Ship>(p, out);       break;
        case Variant::SinZ:       render_variant_fp32<Variant::SinZ>(p, out);       break;
        case Variant::CosZ:       render_variant_fp32<Variant::CosZ>(p, out);       break;
        case Variant::ExpZ:       render_variant_fp32<Variant::ExpZ>(p, out);       break;
        case Variant::SinhZ:      render_variant_fp32<Variant::SinhZ>(p, out);      break;
        case Variant::CoshZ:      render_variant_fp32<Variant::CoshZ>(p, out);      break;
        case Variant::TanZ:       render_variant_fp32<Variant::TanZ>(p, out);       break;
        default: break;
    }
}

static void dispatch_fp64(const MapParams& p, cv::Mat& out) {
    switch (p.variant) {
        case Variant::Mandelbrot: render_variant<Variant::Mandelbrot>(p, out); break;
        case Variant::Tri:        render_variant<Variant::Tri>(p, out);        break;
        case Variant::Boat:       render_variant<Variant::Boat>(p, out);       break;
        case Variant::Duck:       render_variant<Variant::Duck>(p, out);       break;
        case Variant::Bell:       render_variant<Variant::Bell>(p, out);       break;
        case Variant::Fish:       render_variant<Variant::Fish>(p, out);       break;
        case Variant::Vase:       render_variant<Variant::Vase>(p, out);       break;
        case Variant::Bird:       render_variant<Variant::Bird>(p, out);       break;
        case Variant::Mask:       render_variant<Variant::Mask>(p, out);       break;
        case Variant::Ship:       render_variant<Variant::Ship>(p, out);       break;
        case Variant::SinZ:       render_variant<Variant::SinZ>(p, out);       break;
        case Variant::CosZ:       render_variant<Variant::CosZ>(p, out);       break;
        case Variant::ExpZ:       render_variant<Variant::ExpZ>(p, out);       break;
        case Variant::SinhZ:      render_variant<Variant::SinhZ>(p, out);      break;
        case Variant::CoshZ:      render_variant<Variant::CoshZ>(p, out);      break;
        case Variant::TanZ:       render_variant<Variant::TanZ>(p, out);       break;
        default: break;  // Variant::Custom is intercepted before this dispatch
    }
}

static void dispatch_fp80(const MapParams& p, cv::Mat& out) {
    switch (p.variant) {
        case Variant::Mandelbrot: render_variant_fp80<Variant::Mandelbrot>(p, out); break;
        case Variant::Tri:        render_variant_fp80<Variant::Tri>(p, out);        break;
        case Variant::Boat:       render_variant_fp80<Variant::Boat>(p, out);       break;
        case Variant::Duck:       render_variant_fp80<Variant::Duck>(p, out);       break;
        case Variant::Bell:       render_variant_fp80<Variant::Bell>(p, out);       break;
        case Variant::Fish:       render_variant_fp80<Variant::Fish>(p, out);       break;
        case Variant::Vase:       render_variant_fp80<Variant::Vase>(p, out);       break;
        case Variant::Bird:       render_variant_fp80<Variant::Bird>(p, out);       break;
        case Variant::Mask:       render_variant_fp80<Variant::Mask>(p, out);       break;
        case Variant::Ship:       render_variant_fp80<Variant::Ship>(p, out);       break;
        case Variant::SinZ:       render_variant_fp80<Variant::SinZ>(p, out);       break;
        case Variant::CosZ:       render_variant_fp80<Variant::CosZ>(p, out);       break;
        case Variant::ExpZ:       render_variant_fp80<Variant::ExpZ>(p, out);       break;
        case Variant::SinhZ:      render_variant_fp80<Variant::SinhZ>(p, out);      break;
        case Variant::CoshZ:      render_variant_fp80<Variant::CoshZ>(p, out);      break;
        case Variant::TanZ:       render_variant_fp80<Variant::TanZ>(p, out);       break;
        default: break;  // Variant::Custom is intercepted before this dispatch
    }
}

#if defined(FSD_HAS_FLOAT128)
static void dispatch_fp128(const MapParams& p, cv::Mat& out) {
    switch (p.variant) {
        case Variant::Mandelbrot: render_variant_fp128<Variant::Mandelbrot>(p, out); break;
        case Variant::Tri:        render_variant_fp128<Variant::Tri>(p, out);        break;
        case Variant::Boat:       render_variant_fp128<Variant::Boat>(p, out);       break;
        case Variant::Duck:       render_variant_fp128<Variant::Duck>(p, out);       break;
        case Variant::Bell:       render_variant_fp128<Variant::Bell>(p, out);       break;
        case Variant::Fish:       render_variant_fp128<Variant::Fish>(p, out);       break;
        case Variant::Vase:       render_variant_fp128<Variant::Vase>(p, out);       break;
        case Variant::Bird:       render_variant_fp128<Variant::Bird>(p, out);       break;
        case Variant::Mask:       render_variant_fp128<Variant::Mask>(p, out);       break;
        case Variant::Ship:       render_variant_fp128<Variant::Ship>(p, out);       break;
        case Variant::SinZ:       render_variant_fp128<Variant::SinZ>(p, out);       break;
        case Variant::CosZ:       render_variant_fp128<Variant::CosZ>(p, out);       break;
        case Variant::ExpZ:       render_variant_fp128<Variant::ExpZ>(p, out);       break;
        case Variant::SinhZ:      render_variant_fp128<Variant::SinhZ>(p, out);      break;
        case Variant::CoshZ:      render_variant_fp128<Variant::CoshZ>(p, out);      break;
        case Variant::TanZ:       render_variant_fp128<Variant::TanZ>(p, out);       break;
        default: break;  // Variant::Custom is intercepted before this dispatch
    }
}
#endif

// Dispatch fixed-point variants (trig variants fall back to fp64).
template <int FRAC>
static void dispatch_fixed(const MapParams& p, cv::Mat& out) {
    switch (p.variant) {
        case Variant::Mandelbrot: render_variant_fixed<FRAC, Variant::Mandelbrot>(p, out); break;
        case Variant::Tri:        render_variant_fixed<FRAC, Variant::Tri>(p, out);        break;
        case Variant::Boat:       render_variant_fixed<FRAC, Variant::Boat>(p, out);       break;
        case Variant::Duck:       render_variant_fixed<FRAC, Variant::Duck>(p, out);       break;
        case Variant::Bell:       render_variant_fixed<FRAC, Variant::Bell>(p, out);       break;
        case Variant::Fish:       render_variant_fixed<FRAC, Variant::Fish>(p, out);       break;
        case Variant::Vase:       render_variant_fixed<FRAC, Variant::Vase>(p, out);       break;
        case Variant::Bird:       render_variant_fixed<FRAC, Variant::Bird>(p, out);       break;
        case Variant::Mask:       render_variant_fixed<FRAC, Variant::Mask>(p, out);       break;
        case Variant::Ship:       render_variant_fixed<FRAC, Variant::Ship>(p, out);       break;
        case Variant::SinZ:
        case Variant::CosZ:
        case Variant::ExpZ:
        case Variant::SinhZ:
        case Variant::CoshZ:
        case Variant::TanZ:
            dispatch_fp64(p, out);
            break;
        default: break;  // Variant::Custom is intercepted before this dispatch
    }
}

// ─── Custom variant renderer (OpenMP, function pointer) ──────────────────────
//
// Only used when p.variant == Variant::Custom && p.custom_step_fn != nullptr.
// Falls back to escape metric colorization (min_abs/max_abs/envelope/pairwise
// are not tracked since the custom step fn has no access to orbit buffers).

static MapStats render_custom_openmp(const MapParams& p, cv::Mat& out) {
    if (out.empty() || out.rows != p.height || out.cols != p.width || out.type() != CV_8UC3) {
        out.create(p.height, p.width, CV_8UC3);
    }

    CustomStepFn fn = p.custom_step_fn;

    const int W = p.width, H = p.height;
    const double aspect  = static_cast<double>(W) / static_cast<double>(H);
    const double span_im = p.scale;
    const double span_re = p.scale * aspect;
    const double re_min  = p.center_re - span_re * 0.5;
    const double im_max  = p.center_im + span_im * 0.5;
    const double bail2   = p.bailout_sq;
    const double jre     = p.julia_re;
    const double jim     = p.julia_im;
    const int thread_count = resolve_render_threads(p.render_threads);
    constexpr int tile_size = 32;
    const int tiles_x = ceil_div(W, tile_size);
    const int tiles_y = ceil_div(H, tile_size);
    const int tile_count = tiles_x * tiles_y;
    std::atomic<bool> cancelled{false};

    const auto t0 = std::chrono::steady_clock::now();

    #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 1)
    for (int tile = 0; tile < tile_count; tile++) {
        if (mark_cancelled_if_requested(p, cancelled)) continue;
        const int tile_x = tile % tiles_x;
        const int tile_y = tile / tiles_x;
        const int x0 = tile_x * tile_size;
        const int y0 = tile_y * tile_size;
        const int x1 = std::min(W, x0 + tile_size);
        const int y1 = std::min(H, y0 + tile_size);

        for (int y = y0; y < y1; y++) {
            if (mark_cancelled_if_requested(p, cancelled)) break;
            uint8_t* row = out.ptr<uint8_t>(y);
            const double im_c = im_max - (static_cast<double>(y) + 0.5) / H * span_im;
            for (int x = x0; x < x1; x++) {
                const double re_c = re_min + (static_cast<double>(x) + 0.5) / W * span_re;

                double zr, zi, cr, ci;
                if (p.julia) { zr = re_c; zi = im_c; cr = jre; ci = jim; }
                else          { zr = 0.0; zi = 0.0;  cr = re_c; ci = im_c; }

                int   it    = 0;
                double norm2 = 0.0;
                for (; it < p.iterations; it++) {
                    double nr = 0.0, ni = 0.0;
                    fn(zr, zi, cr, ci, &nr, &ni);
                    zr = nr; zi = ni;
                    const bool finite_z = std::isfinite(zr) && std::isfinite(zi);
                    norm2 = finite_z ? (zr * zr + zi * zi)
                                     : std::numeric_limits<double>::infinity();
                    if (!finite_z || norm2 > bail2) break;
                }

                const bool escaped = (norm2 > bail2);
                uint8_t* px = row + 3 * x;
                colorize_escape_bgr(
                    escaped ? it : p.iterations,
                    p.iterations,
                    p.colormap,
                    escaped ? norm2 : 0.0,
                    p.smooth,
                    px[0], px[1], px[2]
                );
            }
        }
    }

    throw_if_cancelled(p, cancelled);

    const auto t1 = std::chrono::steady_clock::now();
    MapStats s;
    s.elapsed_ms  = std::chrono::duration<double, std::milli>(t1 - t0).count();
    s.pixel_count = W * H;
    s.scalar_used = "fp64";
    s.engine_used = "openmp";
    return s;
}

// Field variant for custom formula — fills FieldOutput with escape metric data.
static MapStats render_custom_field_openmp(const MapParams& p, FieldOutput& fo) {
    fo.width  = p.width;
    fo.height = p.height;
    fo.metric = Metric::Escape;  // custom always uses escape metric for field

    CustomStepFn fn = p.custom_step_fn;

    const int W = p.width, H = p.height;
    const double aspect  = static_cast<double>(W) / static_cast<double>(H);
    const double span_im = p.scale;
    const double span_re = p.scale * aspect;
    const double re_min  = p.center_re - span_re * 0.5;
    const double im_max  = p.center_im + span_im * 0.5;
    const double bail2   = p.bailout_sq;
    const double jre     = p.julia_re;
    const double jim     = p.julia_im;
    const int thread_count = resolve_render_threads(p.render_threads);
    constexpr int tile_size = 32;
    const int tiles_x = ceil_div(W, tile_size);
    const int tiles_y = ceil_div(H, tile_size);
    const int tile_count = tiles_x * tiles_y;

    fo.iter_u32.assign(static_cast<size_t>(W) * H, 0u);
    fo.norm_f32.assign(static_cast<size_t>(W) * H, 0.0f);

    const auto t0 = std::chrono::steady_clock::now();

    #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 1)
    for (int tile = 0; tile < tile_count; tile++) {
        const int tile_x = tile % tiles_x;
        const int tile_y = tile / tiles_x;
        const int x0 = tile_x * tile_size;
        const int y0 = tile_y * tile_size;
        const int x1 = std::min(W, x0 + tile_size);
        const int y1 = std::min(H, y0 + tile_size);

        for (int y = y0; y < y1; y++) {
            const double im_c = im_max - (static_cast<double>(y) + 0.5) / H * span_im;
            for (int x = x0; x < x1; x++) {
                const double re_c = re_min + (static_cast<double>(x) + 0.5) / W * span_re;

                double zr, zi, cr, ci;
                if (p.julia) { zr = re_c; zi = im_c; cr = jre; ci = jim; }
                else          { zr = 0.0; zi = 0.0;  cr = re_c; ci = im_c; }

                int   it    = 0;
                double norm2 = 0.0;
                for (; it < p.iterations; it++) {
                    double nr = 0.0, ni = 0.0;
                    fn(zr, zi, cr, ci, &nr, &ni);
                    zr = nr; zi = ni;
                    const bool finite_z = std::isfinite(zr) && std::isfinite(zi);
                    norm2 = finite_z ? (zr * zr + zi * zi)
                                     : std::numeric_limits<double>::infinity();
                    if (!finite_z || norm2 > bail2) break;
                }

                const bool escaped = (norm2 > bail2);
                const size_t idx = static_cast<size_t>(y) * W + x;
                fo.iter_u32[idx] = escaped ? static_cast<uint32_t>(it) : static_cast<uint32_t>(p.iterations);
                fo.norm_f32[idx] = escaped ? static_cast<float>(norm2) : 0.0f;
            }
        }
    }

    const auto t1 = std::chrono::steady_clock::now();
    MapStats s;
    s.elapsed_ms  = std::chrono::duration<double, std::milli>(t1 - t0).count();
    s.pixel_count = W * H;
    s.scalar_used = "fp64";
    s.engine_used = "openmp";
    fo.scalar_used  = "fp64";
    fo.engine_used  = "openmp";
    return s;
}

// ─── Raw field renderer (always OpenMP, no colorization) ─────────────────────

namespace {

template <Variant V, typename S, Metric M, IterResultMask NeedMask>
void field_variant_impl(const MapParams& p, FieldOutput& out) {
    const int W = p.width;
    const int H = p.height;
    const double aspect  = static_cast<double>(W) / static_cast<double>(H);
    const double span_im = p.scale;
    const double span_re = p.scale * aspect;
    const double re_min  = p.center_re - span_re * 0.5;
    const double im_max  = p.center_im + span_im * 0.5;
    const double bail2   = p.bailout_sq;

    const S jre = scalar_from_double<S>(p.julia_re);
    const S jim = scalar_from_double<S>(p.julia_im);
    const Cx<S> c_const{jre, jim};

    const int thread_count = resolve_render_threads(p.render_threads);
    constexpr bool is_escape = (M == Metric::Escape);
    constexpr int tile_size = 32;
    const int tiles_x = ceil_div(W, tile_size);
    const int tiles_y = ceil_div(H, tile_size);
    const int tile_count = tiles_x * tiles_y;

    if constexpr (is_escape) {
        out.iter_u32.assign(static_cast<size_t>(W) * H, 0u);
        out.norm_f32.assign(static_cast<size_t>(W) * H, 0.0f);
    } else {
        out.field_f64.assign(static_cast<size_t>(W) * H, 0.0);
    }

    #pragma omp parallel num_threads(thread_count)
    {
        std::vector<Cx<S>> orbit;
        if constexpr (M == Metric::MinPairwiseDist) {
            orbit.reserve(static_cast<size_t>(p.pairwise_cap));
        }

        #pragma omp for schedule(dynamic, 1)
        for (int tile = 0; tile < tile_count; tile++) {
            const int tile_x = tile % tiles_x;
            const int tile_y = tile / tiles_x;
            const int x0 = tile_x * tile_size;
            const int y0 = tile_y * tile_size;
            const int x1 = std::min(W, x0 + tile_size);
            const int y1 = std::min(H, y0 + tile_size);

            for (int y = y0; y < y1; y++) {
                const double im_d = im_max - (static_cast<double>(y) + 0.5) / H * span_im;
                const S im = scalar_from_double<S>(im_d);
                for (int x = x0; x < x1; x++) {
                    const double re_d = re_min + (static_cast<double>(x) + 0.5) / W * span_re;
                    const S re = scalar_from_double<S>(re_d);

                    Cx<S> z0, c;
                    if (p.julia) {
                        z0 = Cx<S>{re, im};
                        c  = c_const;
                    } else {
                        z0 = Cx<S>{scalar_from_double<S>(0.0), scalar_from_double<S>(0.0)};
                        c  = Cx<S>{re, im};
                    }

                    IterResult r;
                    if constexpr (M == Metric::MinPairwiseDist) {
                        r = iterate_pairwise<V, S>(
                            z0, c, p.iterations, p.bailout, bail2, p.pairwise_cap, orbit);
                    } else {
                        r = iterate_masked<NeedMask, V, S>(
                            z0, c, p.iterations, p.bailout, bail2);
                    }

                    const size_t idx = static_cast<size_t>(y) * W + x;
                    if constexpr (is_escape) {
                        out.iter_u32[idx] = r.escaped
                            ? static_cast<uint32_t>(r.iter)
                            : static_cast<uint32_t>(p.iterations);
                        out.norm_f32[idx] = r.escaped ? static_cast<float>(r.norm) : 0.0f;
                    } else {
                        out.field_f64[idx] = raw_field_value<M>(r);
                    }
                }
            }
        }
    }

    if constexpr (!is_escape) {
        double lo =  std::numeric_limits<double>::infinity();
        double hi = -std::numeric_limits<double>::infinity();
        for (double v : out.field_f64) {
            if (std::isfinite(v)) {
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
        }
        out.field_min = std::isfinite(lo) ? lo : 0.0;
        out.field_max = std::isfinite(hi) ? hi : 1.0;
    }
}

template <int FRAC, Variant V, Metric M, IterResultMask NeedMask>
void field_variant_fixed_impl(const MapParams& p, FieldOutput& out) {
    static_assert(!variant_is_transcendental_v<V>(),
        "Fixed-point integer field renderer only supports quadratic variants.");
    static_assert(M != Metric::MinPairwiseDist,
        "Fixed-point integer field renderer does not handle pairwise distance.");

    using S = Fixed64<FRAC>;
    const int W = p.width;
    const int H = p.height;
    const FixedViewportRaw<FRAC> vp = make_fixed_viewport_raw<FRAC>(
        p.center_re, p.center_im, p.scale, W, H,
        p.julia_re, p.julia_im, p.bailout, p.bailout_sq);

    const int thread_count = resolve_render_threads(p.render_threads);
    constexpr bool is_escape = (M == Metric::Escape);
    constexpr int tile_size = 32;
    const int tiles_x = ceil_div(W, tile_size);
    const int tiles_y = ceil_div(H, tile_size);
    const int tile_count = tiles_x * tiles_y;

    if constexpr (is_escape) {
        out.iter_u32.assign(static_cast<size_t>(W) * H, 0u);
        out.norm_f32.assign(static_cast<size_t>(W) * H, 0.0f);
    } else {
        out.field_f64.assign(static_cast<size_t>(W) * H, 0.0);
    }

    const Cx<S> c_const{S{vp.julia_re_raw}, S{vp.julia_im_raw}};

    #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 1)
    for (int tile = 0; tile < tile_count; tile++) {
        const int tile_x = tile % tiles_x;
        const int tile_y = tile / tiles_x;
        const int x0 = tile_x * tile_size;
        const int y0 = tile_y * tile_size;
        const int x1 = std::min(W, x0 + tile_size);
        const int y1 = std::min(H, y0 + tile_size);

        for (int y = y0; y < y1; y++) {
            const S im{fixed_pixel_im_raw<FRAC>(vp, y)};
            for (int x = x0; x < x1; x++) {
                const S re{fixed_pixel_re_raw<FRAC>(vp, x)};

                Cx<S> z0, c;
                if (p.julia) {
                    z0 = Cx<S>{re, im};
                    c  = c_const;
                } else {
                    z0 = Cx<S>{S{INT64_C(0)}, S{INT64_C(0)}};
                    c  = Cx<S>{re, im};
                }

                constexpr FixedEscapeGate gate = FRAC == 60
                    ? FixedEscapeGate::L1
                    : (FRAC == 59 ? FixedEscapeGate::Component : FixedEscapeGate::Direct);
                const IterResult r = iterate_fixed_int_masked<FRAC, gate, NeedMask, V>(
                    z0, c, p.iterations, vp.bailout_raw, vp.bailout2_raw,
                    vp.two_raw, vp.two_sqrt2_floor_raw, p.julia);

                const size_t idx = static_cast<size_t>(y) * W + x;
                if constexpr (is_escape) {
                    out.iter_u32[idx] = r.escaped
                        ? static_cast<uint32_t>(r.iter)
                        : static_cast<uint32_t>(p.iterations);
                    out.norm_f32[idx] = r.escaped ? static_cast<float>(r.norm) : 0.0f;
                } else {
                    out.field_f64[idx] = raw_field_value<M>(r);
                }
            }
        }
    }

    if constexpr (!is_escape) {
        double lo =  std::numeric_limits<double>::infinity();
        double hi = -std::numeric_limits<double>::infinity();
        for (double v : out.field_f64) {
            if (std::isfinite(v)) {
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
        }
        out.field_min = std::isfinite(lo) ? lo : 0.0;
        out.field_max = std::isfinite(hi) ? hi : 1.0;
    }
}

template <Variant V, typename S>
void field_variant_scalar(const MapParams& p, FieldOutput& out) {
    switch (p.metric) {
        case Metric::Escape:
            field_variant_impl<V, S, Metric::Escape, NeedEscapeSmooth>(p, out); break;
        case Metric::MinAbs:
            field_variant_impl<V, S, Metric::MinAbs, NeedMinAbs>(p, out); break;
        case Metric::MaxAbs:
            field_variant_impl<V, S, Metric::MaxAbs, NeedMaxAbs>(p, out); break;
        case Metric::Envelope:
            field_variant_impl<V, S, Metric::Envelope, NeedEnvelope>(p, out); break;
        case Metric::MinPairwiseDist:
            field_variant_impl<V, S, Metric::MinPairwiseDist, IterResultField::Extra>(p, out); break;
    }
}

template <Variant V>
void field_variant_fp64(const MapParams& p, FieldOutput& out) {
    field_variant_scalar<V, double>(p, out);
}

template <Variant V>
void field_variant_fp80(const MapParams& p, FieldOutput& out) {
    field_variant_scalar<V, long double>(p, out);
}

#if defined(FSD_HAS_FLOAT128)
template <Variant V>
void field_variant_fp128(const MapParams& p, FieldOutput& out) {
    field_variant_scalar<V, __float128>(p, out);
}
#endif

template <int FRAC, Variant V>
void field_variant_fixed(const MapParams& p, FieldOutput& out) {
    static_assert(!variant_is_transcendental_v<V>(),
        "Fixed-point integer field renderer only supports quadratic variants.");
    switch (p.metric) {
        case Metric::Escape:
            field_variant_fixed_impl<FRAC, V, Metric::Escape, NeedEscapeSmooth>(p, out); break;
        case Metric::MinAbs:
            field_variant_fixed_impl<FRAC, V, Metric::MinAbs, NeedMinAbs>(p, out); break;
        case Metric::MaxAbs:
            field_variant_fixed_impl<FRAC, V, Metric::MaxAbs, NeedMaxAbs>(p, out); break;
        case Metric::Envelope:
            field_variant_fixed_impl<FRAC, V, Metric::Envelope, NeedEnvelope>(p, out); break;
        case Metric::MinPairwiseDist:
            field_variant_impl<V, double, Metric::MinPairwiseDist, IterResultField::Extra>(p, out); break;
    }
}

void dispatch_field_fp64(const MapParams& p, FieldOutput& out) {
    switch (p.variant) {
        case Variant::Mandelbrot: field_variant_fp64<Variant::Mandelbrot>(p, out); break;
        case Variant::Tri:        field_variant_fp64<Variant::Tri>       (p, out); break;
        case Variant::Boat:       field_variant_fp64<Variant::Boat>      (p, out); break;
        case Variant::Duck:       field_variant_fp64<Variant::Duck>      (p, out); break;
        case Variant::Bell:       field_variant_fp64<Variant::Bell>      (p, out); break;
        case Variant::Fish:       field_variant_fp64<Variant::Fish>      (p, out); break;
        case Variant::Vase:       field_variant_fp64<Variant::Vase>      (p, out); break;
        case Variant::Bird:       field_variant_fp64<Variant::Bird>      (p, out); break;
        case Variant::Mask:       field_variant_fp64<Variant::Mask>      (p, out); break;
        case Variant::Ship:       field_variant_fp64<Variant::Ship>      (p, out); break;
        case Variant::SinZ:       field_variant_fp64<Variant::SinZ>      (p, out); break;
        case Variant::CosZ:       field_variant_fp64<Variant::CosZ>      (p, out); break;
        case Variant::ExpZ:       field_variant_fp64<Variant::ExpZ>      (p, out); break;
        case Variant::SinhZ:      field_variant_fp64<Variant::SinhZ>     (p, out); break;
        case Variant::CoshZ:      field_variant_fp64<Variant::CoshZ>     (p, out); break;
        case Variant::TanZ:       field_variant_fp64<Variant::TanZ>      (p, out); break;
        default: break;  // Variant::Custom intercepted before this dispatch
    }
}

void dispatch_field_fp80(const MapParams& p, FieldOutput& out) {
    switch (p.variant) {
        case Variant::Mandelbrot: field_variant_fp80<Variant::Mandelbrot>(p, out); break;
        case Variant::Tri:        field_variant_fp80<Variant::Tri>       (p, out); break;
        case Variant::Boat:       field_variant_fp80<Variant::Boat>      (p, out); break;
        case Variant::Duck:       field_variant_fp80<Variant::Duck>      (p, out); break;
        case Variant::Bell:       field_variant_fp80<Variant::Bell>      (p, out); break;
        case Variant::Fish:       field_variant_fp80<Variant::Fish>      (p, out); break;
        case Variant::Vase:       field_variant_fp80<Variant::Vase>      (p, out); break;
        case Variant::Bird:       field_variant_fp80<Variant::Bird>      (p, out); break;
        case Variant::Mask:       field_variant_fp80<Variant::Mask>      (p, out); break;
        case Variant::Ship:       field_variant_fp80<Variant::Ship>      (p, out); break;
        case Variant::SinZ:       field_variant_fp80<Variant::SinZ>      (p, out); break;
        case Variant::CosZ:       field_variant_fp80<Variant::CosZ>      (p, out); break;
        case Variant::ExpZ:       field_variant_fp80<Variant::ExpZ>      (p, out); break;
        case Variant::SinhZ:      field_variant_fp80<Variant::SinhZ>     (p, out); break;
        case Variant::CoshZ:      field_variant_fp80<Variant::CoshZ>     (p, out); break;
        case Variant::TanZ:       field_variant_fp80<Variant::TanZ>      (p, out); break;
        default: break;  // Variant::Custom intercepted before this dispatch
    }
}

#if defined(FSD_HAS_FLOAT128)
void dispatch_field_fp128(const MapParams& p, FieldOutput& out) {
    switch (p.variant) {
        case Variant::Mandelbrot: field_variant_fp128<Variant::Mandelbrot>(p, out); break;
        case Variant::Tri:        field_variant_fp128<Variant::Tri>       (p, out); break;
        case Variant::Boat:       field_variant_fp128<Variant::Boat>      (p, out); break;
        case Variant::Duck:       field_variant_fp128<Variant::Duck>      (p, out); break;
        case Variant::Bell:       field_variant_fp128<Variant::Bell>      (p, out); break;
        case Variant::Fish:       field_variant_fp128<Variant::Fish>      (p, out); break;
        case Variant::Vase:       field_variant_fp128<Variant::Vase>      (p, out); break;
        case Variant::Bird:       field_variant_fp128<Variant::Bird>      (p, out); break;
        case Variant::Mask:       field_variant_fp128<Variant::Mask>      (p, out); break;
        case Variant::Ship:       field_variant_fp128<Variant::Ship>      (p, out); break;
        case Variant::SinZ:       field_variant_fp128<Variant::SinZ>      (p, out); break;
        case Variant::CosZ:       field_variant_fp128<Variant::CosZ>      (p, out); break;
        case Variant::ExpZ:       field_variant_fp128<Variant::ExpZ>      (p, out); break;
        case Variant::SinhZ:      field_variant_fp128<Variant::SinhZ>     (p, out); break;
        case Variant::CoshZ:      field_variant_fp128<Variant::CoshZ>     (p, out); break;
        case Variant::TanZ:       field_variant_fp128<Variant::TanZ>      (p, out); break;
        default: break;  // Variant::Custom intercepted before this dispatch
    }
}
#endif

template <int FRAC>
void dispatch_field_fixed(const MapParams& p, FieldOutput& out) {
    switch (p.variant) {
        case Variant::Mandelbrot: field_variant_fixed<FRAC, Variant::Mandelbrot>(p, out); break;
        case Variant::Tri:        field_variant_fixed<FRAC, Variant::Tri>       (p, out); break;
        case Variant::Boat:       field_variant_fixed<FRAC, Variant::Boat>      (p, out); break;
        case Variant::Duck:       field_variant_fixed<FRAC, Variant::Duck>      (p, out); break;
        case Variant::Bell:       field_variant_fixed<FRAC, Variant::Bell>      (p, out); break;
        case Variant::Fish:       field_variant_fixed<FRAC, Variant::Fish>      (p, out); break;
        case Variant::Vase:       field_variant_fixed<FRAC, Variant::Vase>      (p, out); break;
        case Variant::Bird:       field_variant_fixed<FRAC, Variant::Bird>      (p, out); break;
        case Variant::Mask:       field_variant_fixed<FRAC, Variant::Mask>      (p, out); break;
        case Variant::Ship:       field_variant_fixed<FRAC, Variant::Ship>      (p, out); break;
        case Variant::SinZ:
        case Variant::CosZ:
        case Variant::ExpZ:
        case Variant::SinhZ:
        case Variant::CoshZ:
        case Variant::TanZ:
            dispatch_field_fp64(p, out);
            break;
        default: break;  // Variant::Custom intercepted before this dispatch
    }
}

} // anonymous namespace (field kernels)

// ── Burning-Ship guided exploration ──────────────────────────────────────────
// The Burning-Ship step (|x|+i|y|)²+c and the Mandelbrot step z²+c share the real part each
// iteration; their imaginary parts (2·|x|·|y| vs 2·x·y) agree iff x·y ≥ 0, so the two orbits
// coincide until the first iterate with x·y < 0. This mode renders the *actual* Burning Ship and
// dims/desaturates the pixels whose orbit never diverges from the Mandelbrot's — so the
// ship-specific structure (where the abs() folds carve new shapes) stands out in full colour,
// guiding exploration toward what is unique to the Burning Ship. Scalar-OpenMP (an overlay).
namespace {

// One Burning-Ship orbit, also tracking how long it stayed identical to the Mandelbrot
// (x·y ≥ 0 throughout — the two are bit-identical up to the first x·y < 0). Returns the ship
// escape iteration (== max_iter if bounded), the agreement length, and whether it never diverged.
struct ShipExplore { int iter; int agree; bool fully; };

inline ShipExplore ship_explore_orbit(double zx, double zy, double cx, double cy,
                                      int max_iter, double bail2) {
    int agree = 0;
    bool diverged = false;
    int i = 0;
    for (; i < max_iter; ++i) {
        if (!diverged) {
            if (zx * zy < 0.0) diverged = true;     // the ship's abs() changes the orbit here
            else               agree = i + 1;
        }
        const double ax = std::fabs(zx), ay = std::fabs(zy);
        const double nx = ax * ax - ay * ay + cx;   // = zx²-zy²+cx  (shared real part)
        const double ny = 2.0 * ax * ay + cy;       // Burning-Ship imaginary part
        zx = nx; zy = ny;
        if (zx * zx + zy * zy > bail2) break;        // escaped (i = escape iteration)
    }
    return { i, agree, !diverged };
}

// Mandelbrot-likeness in [0,1]: the FRACTION of this pixel's orbit that stayed identical to the
// Mandelbrot (x·y ≥ 0). 1 = never diverged (bit-identical image); near 1 = diverged only at the
// very end (escape time barely changed); near 0 = the abs() folds mattered immediately. Using the
// orbit fraction (not absolute length) means escaped pixels are graded too, not just the interior.
inline double ship_mark(const ShipExplore& e) {
    if (e.fully) return 1.0;
    const double f = std::min(1.0, static_cast<double>(e.agree) / static_cast<double>(std::max(1, e.iter)));
    return std::pow(f, 1.5);
}

} // anonymous namespace

static MapStats render_mandel_ship_agree(const MapParams& p, cv::Mat& out) {
    if (out.empty() || out.rows != p.height || out.cols != p.width || out.type() != CV_8UC3) {
        out.create(p.height, p.width, CV_8UC3);
    }
    const int W = p.width, H = p.height;
    const double aspect = static_cast<double>(W) / static_cast<double>(H);
    const double span_im = p.scale, span_re = p.scale * aspect;
    const double re_min = p.center_re - span_re * 0.5;
    const double im_max = p.center_im + span_im * 0.5;
    const double bail2 = p.bailout_sq;
    const int max_iter = p.iterations;
    const int thread_count = resolve_render_threads(p.render_threads);

    const auto t0 = std::chrono::steady_clock::now();
    #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 8)
    for (int y = 0; y < H; ++y) {
        if (map_render_cancel_requested(p)) continue;
        const double im = im_max - (static_cast<double>(y) + 0.5) / H * span_im;
        uint8_t* row = out.ptr<uint8_t>(y);
        for (int x = 0; x < W; ++x) {
            const double re = re_min + (static_cast<double>(x) + 0.5) / W * span_re;
            double zx, zy, cx, cy;
            if (p.julia) { zx = re; zy = im; cx = p.julia_re; cy = p.julia_im; }
            else         { zx = 0.0; zy = 0.0; cx = re; cy = im; }
            const ShipExplore e = ship_explore_orbit(zx, zy, cx, cy, max_iter, bail2);

            uint8_t b, g, r;
            colorize_escape_bgr(e.iter, max_iter, p.colormap, 0.0, false, b, g, r);

            // Mark Mandelbrot-identical pixels by shifting every channel (x+128)%256 — a high-
            // contrast recolour that's hard to confuse with the original. Graded by how much of
            // the orbit matched: full +128 where identical, scaling to 0 where the abs() folds
            // diverged immediately (ship-specific keeps its original colour).
            const double mark = ship_mark(e);
            const int shift = static_cast<int>(std::lround(128.0 * mark));
            uint8_t* px = row + 3 * x;
            px[0] = static_cast<uint8_t>((static_cast<int>(b) + shift) & 255);
            px[1] = static_cast<uint8_t>((static_cast<int>(g) + shift) & 255);
            px[2] = static_cast<uint8_t>((static_cast<int>(r) + shift) & 255);
        }
    }
    if (map_render_cancel_requested(p)) throw_render_cancelled();
    const auto t1 = std::chrono::steady_clock::now();
    MapStats s;
    s.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    s.pixel_count = W * H;
    s.scalar_used = "fp64";
    s.engine_used = "openmp";
    return s;
}

static MapStats render_mandel_ship_agree_field(const MapParams& p, FieldOutput& fo) {
    const int W = p.width, H = p.height;
    fo.width = W;
    fo.height = H;
    fo.metric = Metric::MandelShipAgree;
    fo.field_f64.assign(static_cast<size_t>(W) * static_cast<size_t>(H), 0.0);
    const double aspect = static_cast<double>(W) / static_cast<double>(H);
    const double span_im = p.scale, span_re = p.scale * aspect;
    const double re_min = p.center_re - span_re * 0.5;
    const double im_max = p.center_im + span_im * 0.5;
    const double bail2 = p.bailout_sq;
    const int max_iter = p.iterations;
    const int thread_count = resolve_render_threads(p.render_threads);

    const auto t0 = std::chrono::steady_clock::now();
    #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 8)
    for (int y = 0; y < H; ++y) {
        if (map_render_cancel_requested(p)) continue;
        const double im = im_max - (static_cast<double>(y) + 0.5) / H * span_im;
        for (int x = 0; x < W; ++x) {
            const double re = re_min + (static_cast<double>(x) + 0.5) / W * span_re;
            double zx, zy, cx, cy;
            if (p.julia) { zx = re; zy = im; cx = p.julia_re; cy = p.julia_im; }
            else         { zx = 0.0; zy = 0.0; cx = re; cy = im; }
            const ShipExplore e = ship_explore_orbit(zx, zy, cx, cy, max_iter, bail2);
            fo.field_f64[static_cast<size_t>(y) * static_cast<size_t>(W) + x] = ship_mark(e);
        }
    }
    fo.field_min = 0.0;
    fo.field_max = 1.0;
    const auto t1 = std::chrono::steady_clock::now();
    MapStats s;
    s.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    s.pixel_count = W * H;
    s.scalar_used = "fp64";
    s.engine_used = "openmp";
    fo.scalar_used = "fp64";
    fo.engine_used = "openmp";
    return s;
}

MapStats render_map_field(const MapParams& p, FieldOutput& fo) {
    // Custom variant: use function-pointer path (always OpenMP).
    if (p.variant == Variant::Custom && p.custom_step_fn) {
        return render_custom_field_openmp(p, fo);
    }
    if (p.metric == Metric::MandelShipAgree) {
        return render_mandel_ship_agree_field(p, fo);
    }

    fo.width  = p.width;
    fo.height = p.height;
    fo.metric = p.metric;

    // ── Fast SIMD/CUDA field path ────────────────────────────────────────────────
    // The hot consumer (equalized coloring) reads iter_u32; only the escape metric
    // carries per-pixel counts. Mirror render_map's engine choice. Everything else —
    // non-escape metrics, fp80/fp128, fixed-point (no SIMD field yet), custom/trig, or an
    // explicit "openmp" engine — falls through to the unchanged scalar dispatch below. The
    // field is fp64 by design (the scalar path has no fp32 branch), so fp32 requests run the
    // fp64 kernel, preserving existing output. Engines are tried fastest-first; a stub/
    // unavailable backend is skipped via its availability gate.
    const MapEnginePlan field_plan = select_map_engine_plan(p, /*field_output=*/true);
    const bool can_fast_field =
        !field_plan.scalar_fallback && p.metric == Metric::Escape &&
        !field_plan.fp80 && !field_plan.fp128 && !field_plan.fx &&
        field_plan.engine != "openmp";
    if (can_fast_field) {
        auto accept = [&](MapStats& s, const char* eng) {
            if (s.engine_used != eng) return false;
            s.pixel_count = p.width * p.height;
            fo.scalar_used = s.scalar_used;
            fo.engine_used = s.engine_used;
            return true;
        };
#if USE_CUDA
        // Honor an explicitly/benchmark-selected CUDA field (fp32/fp64). On failure or a
        // non-cuda result, fall through to the CPU SIMD paths below.
        if (field_plan.engine == "cuda" && fsd_cuda::cuda_available()) {
            try {
                fsd_cuda::CudaMapParams cp;
                cp.center_re = p.center_re;
                cp.center_im = p.center_im;
                cp.scale     = p.scale;
                cp.width     = p.width;
                cp.height    = p.height;
                cp.iterations = p.iterations;
                cp.bailout    = p.bailout;
                cp.bailout_sq = p.bailout_sq;
                cp.scalar_type = field_plan.fp32 ? "fp32" : "fp64";
                cp.colormap_id = static_cast<int>(p.colormap);
                cp.variant_id  = static_cast<int>(p.variant);
                cp.julia       = p.julia;
                cp.julia_re    = p.julia_re;
                cp.julia_im    = p.julia_im;
                cp.metric_id   = 0;  // escape
                fo.iter_u32.assign(static_cast<size_t>(p.width) * static_cast<size_t>(p.height), 0u);
                fo.norm_f32.assign(static_cast<size_t>(p.width) * static_cast<size_t>(p.height), 0.0f);
                auto cs = fsd_cuda::cuda_render_map_field(cp, fo.iter_u32.data(), fo.norm_f32.data());
                MapStats s;
                s.elapsed_ms  = cs.elapsed_ms;
                s.pixel_count = p.width * p.height;
                s.scalar_used = cs.scalar_used;
                s.engine_used = "cuda";
                fo.scalar_used = s.scalar_used;
                fo.engine_used = s.engine_used;
                return s;
            } catch (...) {
                // CUDA launch/runtime failure → CPU fallback below.
            }
        }
#endif
        // Honor an explicitly-selected AVX-2 (e.g. AVX-512-less CPUs, path-diff coverage).
        if (field_plan.engine == "avx2" && avx2_available() && fma_available()) {
            auto s = render_map_avx2_field(p, fo);
            if (accept(s, "avx2")) return s;
        }
        // Default fast field: AVX-512 (covers avx512 / hybrid / auto selections).
        if (avx512_available()) {
            auto s = render_map_field_avx512_fp64(p, fo);
            if (accept(s, "avx512")) return s;
        }
        // AVX-512 unavailable → AVX-2.
        if (avx2_available() && fma_available()) {
            auto s = render_map_avx2_field(p, fo);
            if (accept(s, "avx2")) return s;
        }
    }

    const std::string effective_scalar = map_effective_scalar_type(p);
    const bool fp80 = effective_scalar == "fp80";
    const bool fp128 = effective_scalar == "fp128";
    const FixedPrecision fixed_precision = select_fixed_precision(p);
    const bool fx = fixed_precision != FixedPrecision::None &&
                    supports_fixed_int_path(p, true);
    const auto t0 = std::chrono::steady_clock::now();

    if (fx && fixed_precision == FixedPrecision::Q360) {
        dispatch_field_fixed<60>(p, fo);
    } else if (fx && fixed_precision == FixedPrecision::Q459) {
        dispatch_field_fixed<59>(p, fo);
    } else if (fx) {
        dispatch_field_fixed<57>(p, fo);
    } else if (fp80) {
        dispatch_field_fp80(p, fo);
    } else if (fp128) {
#if defined(FSD_HAS_FLOAT128)
        dispatch_field_fp128(p, fo);
#else
        throw std::runtime_error("fp128 scalar path requires __float128/libquadmath support");
#endif
    } else {
        dispatch_field_fp64(p, fo);
    }

    const auto t1 = std::chrono::steady_clock::now();
    MapStats s;
    s.elapsed_ms  = std::chrono::duration<double, std::milli>(t1 - t0).count();
    s.pixel_count = p.width * p.height;
    s.scalar_used = fx ? fixed_precision_name(fixed_precision)
                  : (fp80 ? "fp80" : (fp128 ? "fp128" : "fp64"));
    s.engine_used = "openmp";
    fo.scalar_used = s.scalar_used;
    fo.engine_used = s.engine_used;
    return s;
}

// ─── Colorized map renderer ───────────────────────────────────────────────────

MapStats render_map(const MapParams& p, cv::Mat& out) {
    if (out.empty() || out.rows != p.height || out.cols != p.width || out.type() != CV_8UC3) {
        out.create(p.height, p.width, CV_8UC3);
    }
    if (map_render_cancel_requested(p)) throw_render_cancelled();

    // Custom variant: bypass CUDA/AVX and go straight to OpenMP.
    if (p.variant == Variant::Custom && p.custom_step_fn) {
        return render_custom_openmp(p, out);
    }
    // Burning-Ship guided-exploration overlay: variant-independent, scalar OpenMP only.
    if (p.metric == Metric::MandelShipAgree) {
        return render_mandel_ship_agree(p, out);
    }

    const MapEnginePlan plan = select_map_engine_plan(p, /*field_output=*/false);
    const bool fp32 = plan.fp32;
    const bool fp80 = plan.fp80;
    const bool fp128 = plan.fp128;
    const FixedPrecision fixed_precision = plan.fixed_precision;
    const bool fx = plan.fx;
    const std::string selected_engine = plan.engine;

    if (selected_engine == "hybrid") {
        MapParams hybrid_params = p;
        hybrid_params.scalar_type = fx ? fixed_precision_name(fixed_precision) : "fp64";
        auto ts = render_map_hybrid(hybrid_params, out);
        MapStats s;
        s.elapsed_ms = ts.total_ms;
        s.pixel_count = p.width * p.height;
        s.scalar_used = ts.scalar_used;
        s.engine_used = ts.engine_used;
        return s;
    }

    // Interactive CUDA renders use tiles so stale requests can stop between
    // small kernels instead of waiting for one full-frame kernel to return.
    if (selected_engine == "cuda" && p.should_cancel && map_work_is_large(p)) {
        MapParams hybrid_params = p;
        hybrid_params.scalar_type = fx ? fixed_precision_name(fixed_precision) : "fp64";
        auto ts = render_map_hybrid(hybrid_params, out);
        MapStats s;
        s.elapsed_ms = ts.total_ms;
        s.pixel_count = p.width * p.height;
        s.scalar_used = ts.scalar_used;
        s.engine_used = ts.engine_used;
        return s;
    }

    // smooth coloring needs per-pixel |z|² which the AVX-512/CUDA paths don't track.
    // Fall through to OpenMP which has access to IterResult.norm.
    const bool needs_norm = plan.needs_norm;

    // Trig variants need scalar (std::cmath) — skip AVX-512 and CUDA for them.
    const bool scalar_fallback = plan.scalar_fallback;

    // CUDA path: all 10 polynomial variants, Julia mode, metrics 0-3 (not MinPairwiseDist=4).
    // Trig variants fall to OpenMP (scalar_fallback).
    // smooth coloring (needs IterResult.norm) still falls to OpenMP.
#if USE_CUDA
    const bool can_cuda = !needs_norm
                       && !scalar_fallback
                       && (selected_engine == "cuda")
                       && (static_cast<int>(p.metric) < 4)  // excludes MinPairwiseDist
                       && fsd_cuda::cuda_available();
    if (can_cuda) {
        fsd_cuda::CudaMapParams cp;
        cp.center_re  = p.center_re;
        cp.center_im  = p.center_im;
        cp.scale      = p.scale;
        cp.width      = p.width;
        cp.height     = p.height;
        cp.iterations = p.iterations;
        cp.bailout    = p.bailout;
        cp.bailout_sq = p.bailout_sq;
        cp.scalar_type  = fx ? fixed_precision_name(fixed_precision) : (fp32 ? "fp32" : "fp64");
        cp.colormap_id  = static_cast<int>(p.colormap);
        cp.variant_id   = static_cast<int>(p.variant);
        cp.julia        = p.julia;
        cp.julia_re     = p.julia_re;
        cp.julia_im     = p.julia_im;
        cp.metric_id    = static_cast<int>(p.metric);
        if (fx && fixed_precision == FixedPrecision::Q360) {
            const FixedViewportRaw<60> vp = make_fixed_viewport_raw<60>(
                p.center_re, p.center_im, p.scale, p.width, p.height,
                p.julia_re, p.julia_im, p.bailout, p.bailout_sq);
            cp.fx64_viewport.first_re_raw = vp.first_re_raw;
            cp.fx64_viewport.first_im_raw = vp.first_im_raw;
            cp.fx64_viewport.step_re_raw = vp.step_re_raw;
            cp.fx64_viewport.step_im_raw = vp.step_im_raw;
            cp.fx64_viewport.julia_re_raw = vp.julia_re_raw;
            cp.fx64_viewport.julia_im_raw = vp.julia_im_raw;
            cp.fx64_viewport.bailout_raw = vp.bailout_raw;
            cp.fx64_viewport.bailout2_raw = vp.bailout2_raw;
            cp.fx64_viewport.two_raw = vp.two_raw;
            cp.fx64_viewport.two_sqrt2_floor_raw = vp.two_sqrt2_floor_raw;
            cp.fx64_viewport.bailout2_q57 = vp.bailout2_raw;
        } else if (fx && fixed_precision == FixedPrecision::Q459) {
            const FixedViewportRaw<59> vp = make_fixed_viewport_raw<59>(
                p.center_re, p.center_im, p.scale, p.width, p.height,
                p.julia_re, p.julia_im, p.bailout, p.bailout_sq);
            cp.fx64_viewport.first_re_raw = vp.first_re_raw;
            cp.fx64_viewport.first_im_raw = vp.first_im_raw;
            cp.fx64_viewport.step_re_raw = vp.step_re_raw;
            cp.fx64_viewport.step_im_raw = vp.step_im_raw;
            cp.fx64_viewport.julia_re_raw = vp.julia_re_raw;
            cp.fx64_viewport.julia_im_raw = vp.julia_im_raw;
            cp.fx64_viewport.bailout_raw = vp.bailout_raw;
            cp.fx64_viewport.bailout2_raw = vp.bailout2_raw;
            cp.fx64_viewport.two_raw = vp.two_raw;
            cp.fx64_viewport.two_sqrt2_floor_raw = vp.two_sqrt2_floor_raw;
            cp.fx64_viewport.bailout2_q57 = vp.bailout2_raw;
        } else if (fx) {
            const FixedViewportRaw<57> vp = make_fixed_viewport_raw<57>(
                p.center_re, p.center_im, p.scale, p.width, p.height,
                p.julia_re, p.julia_im, p.bailout, p.bailout_sq);
            cp.fx64_viewport.first_re_raw = vp.first_re_raw;
            cp.fx64_viewport.first_im_raw = vp.first_im_raw;
            cp.fx64_viewport.step_re_raw = vp.step_re_raw;
            cp.fx64_viewport.step_im_raw = vp.step_im_raw;
            cp.fx64_viewport.julia_re_raw = vp.julia_re_raw;
            cp.fx64_viewport.julia_im_raw = vp.julia_im_raw;
            cp.fx64_viewport.bailout_raw = vp.bailout_raw;
            cp.fx64_viewport.bailout2_raw = vp.bailout2_raw;
            cp.fx64_viewport.two_raw = vp.two_raw;
            cp.fx64_viewport.two_sqrt2_floor_raw = vp.two_sqrt2_floor_raw;
            cp.fx64_viewport.bailout2_q57 = vp.bailout2_raw;
        }
        try {
            auto cs = fsd_cuda::cuda_render_map(cp, out);
            MapStats s;
            s.elapsed_ms  = cs.elapsed_ms;
            s.pixel_count = p.width * p.height;
            s.scalar_used = cs.scalar_used;
            s.engine_used = "cuda";
            return s;
        } catch (...) {
            // CUDA launch/runtime failures fall through to the CPU path.
        }
    }
#endif

    // AVX-512 path: all 10 polynomial variants, Julia mode, metrics 0-3 (fp64).
    // Fx64 AVX-512 currently falls back to the OpenMP integer path; the old
    // IFMA route used fp64 escape checks inside the hot loop.
    // MinPairwiseDist (metric 4) is excluded: O(N²) orbit buffer not vectorised.
    // Smooth coloring needs per-pixel norm from IterResult — falls to OpenMP.
    // Trig variants need std::cmath — skip AVX-512 (scalar_fallback).
    const bool can_avx_base = !needs_norm
                           && !scalar_fallback
                           && (selected_engine == "avx512")
                           && (static_cast<int>(p.metric) < 4)
                           && avx512_available();
    const bool can_avx = can_avx_base && !fx;

    if (can_avx && fp32) {
        auto s = render_map_avx512_fp32(p, out);
        s.pixel_count = p.width * p.height;
        s.scalar_used = "fp32";
        s.engine_used = "avx512";
        return s;
    }

    if (can_avx && !fp32) {
        auto s = render_map_avx512_fp64(p, out);
        s.pixel_count = p.width * p.height;
        s.scalar_used = "fp64";
        s.engine_used = "avx512";
        return s;
    }

    // AVX2 path: all 10 polynomial variants, Julia mode, metrics 0-3 (fp64).
    const bool can_avx2 = !needs_norm
                        && !scalar_fallback
                        && (selected_engine == "avx2")
                        && (static_cast<int>(p.metric) < 4)
                        && !fx
                        && avx2_available()
                        && fma_available();
    if (can_avx2 && fp32) {
        auto s = render_map_avx2_fp32(p, out);
        s.pixel_count = p.width * p.height;
        s.scalar_used = "fp32";
        s.engine_used = "avx2";
        return s;
    }
    if (can_avx2 && !fp32) {
        auto s = render_map_avx2_fp64(p, out);
        s.pixel_count = p.width * p.height;
        s.scalar_used = "fp64";
        s.engine_used = "avx2";
        return s;
    }

    const auto t0 = std::chrono::steady_clock::now();

    if (fx && fixed_precision == FixedPrecision::Q360) {
        dispatch_fixed<60>(p, out);
    } else if (fx && fixed_precision == FixedPrecision::Q459) {
        dispatch_fixed<59>(p, out);
    } else if (fx) {
        dispatch_fixed<57>(p, out);
    } else if (fp32) {
        dispatch_fp32(p, out);
    } else if (fp80) {
        dispatch_fp80(p, out);
    } else if (fp128) {
#if defined(FSD_HAS_FLOAT128)
        dispatch_fp128(p, out);
#else
        throw std::runtime_error("fp128 scalar path requires __float128/libquadmath support");
#endif
    } else {
        dispatch_fp64(p, out);
    }

    const auto t1 = std::chrono::steady_clock::now();
    MapStats s;
    s.elapsed_ms   = std::chrono::duration<double, std::milli>(t1 - t0).count();
    s.pixel_count  = p.width * p.height;
    s.scalar_used  = fx ? fixed_precision_name(fixed_precision)
                   : (fp32 ? "fp32" : (fp80 ? "fp80" : (fp128 ? "fp128" : "fp64")));
    s.engine_used  = "openmp";
    return s;
}

} // namespace fsd::compute
