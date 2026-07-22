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
#include "orbit_program.hpp"
#include "colorize.hpp"
#include "map_kernel_avx2.hpp"
#include "map_kernel_avx512.hpp"
#include "engine_select.hpp"
#include "escape_time.hpp"
#include "complex.hpp"
#include "fx64_raw.hpp"
#include "parallel.hpp"
#include "perturbation.hpp"
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

// The normal scalar kernels deliberately keep their compact, branch-free
// per-pixel iteration helpers.  Interactive field sessions are different: a
// stale view must be able to leave an in-flight, high-iteration pixel without
// waiting for the rest of its tile.  Keep the cancellation-aware copies local
// to this translation unit and call them only from the progressive field path,
// so ordinary one-shot renders retain their existing hot loops.
inline constexpr int kProgressiveFieldCancelStride = 512;

template <IterResultMask NeedMask, Variant V, typename S>
bool iterate_quadratic_field_cancellable(
    Cx<S> z,
    const Cx<S>& c,
    int max_iter,
    double bailout_sq,
    const MapParams& p,
    std::atomic<bool>& cancelled,
    IterResult& result
) {
    IterResult r = make_iter_result<NeedMask>();

    S x = z.re;
    S y = z.im;
    S x2 = x * x;
    S y2 = y * y;
    const S bailout_sq_s = scalar_from_double<S>(bailout_sq);

    for (int i = 0; i < max_iter; i++) {
        if ((i & (kProgressiveFieldCancelStride - 1)) == 0 &&
            mark_cancelled_if_requested(p, cancelled)) {
            return false;
        }

        S nx{};
        S ny{};
        step_cached<V, S>(x, y, x2, y2, c.re, c.im, nx, ny);

        const bool finite_z =
            std::isfinite(scalar_to_double(nx)) && std::isfinite(scalar_to_double(ny));
        S nx2{};
        S ny2{};
        S n2_s{};
        double n2 = std::numeric_limits<double>::infinity();
        if (finite_z) {
            nx2 = nx * nx;
            ny2 = ny * ny;
            n2_s = nx2 + ny2;
            n2 = scalar_to_double(n2_s);
        }

        if constexpr (iter_result_wants(NeedMask, IterResultField::MinAbs)) {
            if (n2 < r.min_abs) r.min_abs = n2;
        }
        if constexpr (iter_result_wants(NeedMask, IterResultField::MaxAbs)) {
            if (n2 > r.max_abs) r.max_abs = n2;
        }

        const bool escaped_now = !finite_z || n2_s > bailout_sq_s;
        if (escaped_now) {
            if constexpr (iter_result_wants(NeedMask, IterResultField::Iter))    r.iter = i;
            if constexpr (iter_result_wants(NeedMask, IterResultField::Norm))    r.norm = n2;
            if constexpr (iter_result_wants(NeedMask, IterResultField::Escaped)) r.escaped = true;
            if constexpr (iter_result_wants(NeedMask, IterResultField::MinAbs)) {
                if (r.min_abs != std::numeric_limits<double>::infinity()) r.min_abs = scalar_sqrt(r.min_abs);
            }
            if constexpr (iter_result_wants(NeedMask, IterResultField::MaxAbs)) {
                if (r.max_abs != 0.0) r.max_abs = scalar_sqrt(r.max_abs);
            }
            result = r;
            return true;
        }

        x = nx;
        y = ny;
        x2 = nx2;
        y2 = ny2;
    }

    if constexpr (iter_result_wants(NeedMask, IterResultField::Iter))    r.iter = max_iter;
    if constexpr (iter_result_wants(NeedMask, IterResultField::Escaped)) r.escaped = false;
    if constexpr (iter_result_wants(NeedMask, IterResultField::MinAbs)) {
        if (r.min_abs != std::numeric_limits<double>::infinity()) r.min_abs = scalar_sqrt(r.min_abs);
    }
    if constexpr (iter_result_wants(NeedMask, IterResultField::MaxAbs)) {
        if (r.max_abs != 0.0) r.max_abs = scalar_sqrt(r.max_abs);
    }
    result = r;
    return true;
}

template <IterResultMask NeedMask, Variant V, typename S>
bool iterate_field_cancellable(
    Cx<S> z,
    const Cx<S>& c,
    int max_iter,
    double bailout,
    double bailout_sq,
    const MapParams& p,
    std::atomic<bool>& cancelled,
    IterResult& result
) {
    static_assert(!iter_result_wants(NeedMask, IterResultField::Extra),
        "Use iterate_pairwise_field_cancellable for MinPairwiseDist.");
    static_assert(!is_fixed64_v<S>,
        "Use iterate_fixed_field_cancellable for fixed-point render paths.");

    if constexpr (!variant_is_transcendental_v<V>()) {
        return iterate_quadratic_field_cancellable<NeedMask, V, S>(
            z, c, max_iter, bailout_sq, p, cancelled, result);
    } else {
        IterResult r = make_iter_result<NeedMask>();

        for (int i = 0; i < max_iter; i++) {
            if ((i & (kProgressiveFieldCancelStride - 1)) == 0 &&
                mark_cancelled_if_requested(p, cancelled)) {
                return false;
            }

            z = variant_step<V, S>(z, c);
            const double zre = scalar_to_double(z.re);
            const double zim = scalar_to_double(z.im);
            const bool finite_z = std::isfinite(zre) && std::isfinite(zim);
            const double n2 = finite_z
                ? (zre * zre + zim * zim)
                : std::numeric_limits<double>::infinity();

            if constexpr (iter_result_wants(NeedMask, IterResultField::MinAbs)) {
                if (n2 < r.min_abs) r.min_abs = n2;
            }
            if constexpr (iter_result_wants(NeedMask, IterResultField::MaxAbs)) {
                if (n2 > r.max_abs) r.max_abs = n2;
            }

            const double component = std::max(std::fabs(zre), std::fabs(zim));
            const bool escaped_now = !finite_z || component >= bailout;
            if (escaped_now) {
                if constexpr (iter_result_wants(NeedMask, IterResultField::Iter))    r.iter = i;
                if constexpr (iter_result_wants(NeedMask, IterResultField::Norm))    r.norm = n2;
                if constexpr (iter_result_wants(NeedMask, IterResultField::Escaped)) r.escaped = true;
                if constexpr (iter_result_wants(NeedMask, IterResultField::MinAbs)) {
                    if (r.min_abs != std::numeric_limits<double>::infinity()) r.min_abs = scalar_sqrt(r.min_abs);
                }
                if constexpr (iter_result_wants(NeedMask, IterResultField::MaxAbs)) {
                    if (r.max_abs != 0.0) r.max_abs = scalar_sqrt(r.max_abs);
                }
                result = r;
                return true;
            }
        }

        if constexpr (iter_result_wants(NeedMask, IterResultField::Iter))    r.iter = max_iter;
        if constexpr (iter_result_wants(NeedMask, IterResultField::Escaped)) r.escaped = false;
        if constexpr (iter_result_wants(NeedMask, IterResultField::MinAbs)) {
            if (r.min_abs != std::numeric_limits<double>::infinity()) r.min_abs = scalar_sqrt(r.min_abs);
        }
        if constexpr (iter_result_wants(NeedMask, IterResultField::MaxAbs)) {
            if (r.max_abs != 0.0) r.max_abs = scalar_sqrt(r.max_abs);
        }
        result = r;
        return true;
    }
}

template <Variant V, typename S>
bool iterate_pairwise_field_cancellable(
    Cx<S> z,
    const Cx<S>& c,
    int max_iter,
    double bailout,
    double bailout_sq,
    int pairwise_cap,
    std::vector<Cx<S>>& orbit_scratch,
    const MapParams& p,
    std::atomic<bool>& cancelled,
    IterResult& result
) {
    static_assert(!is_fixed64_v<S>,
        "Fixed-point MinPairwiseDist currently falls back to the fp64 CPU path.");
    IterResult r{};
    r.iter    = 0;
    r.min_abs = std::numeric_limits<double>::infinity();
    r.max_abs = 0.0;
    r.extra   = std::numeric_limits<double>::infinity();
    r.norm    = 0.0;
    r.escaped = false;
    r.valid_mask = IterResultField::Extra;

    if (pairwise_cap <= 0) {
        result = r;
        return true;
    }
    orbit_scratch.clear();
    orbit_scratch.reserve(static_cast<size_t>(pairwise_cap));

    const int n_iter = std::min(max_iter, pairwise_cap);
    for (int i = 0; i < n_iter; i++) {
        if ((i & (kProgressiveFieldCancelStride - 1)) == 0 &&
            mark_cancelled_if_requested(p, cancelled)) {
            return false;
        }

        z = variant_step<V, S>(z, c);
        const double zre = scalar_to_double(z.re);
        const double zim = scalar_to_double(z.im);
        const bool finite_z = std::isfinite(zre) && std::isfinite(zim);
        const double n2 = finite_z
            ? (zre * zre + zim * zim)
            : std::numeric_limits<double>::infinity();

        for (size_t prior_index = 0; prior_index < orbit_scratch.size(); ++prior_index) {
            if ((prior_index & (kProgressiveFieldCancelStride - 1)) == 0 &&
                mark_cancelled_if_requested(p, cancelled)) {
                return false;
            }
            const auto& prior = orbit_scratch[prior_index];
            const double dr = zre - scalar_to_double(prior.re);
            const double di = zim - scalar_to_double(prior.im);
            const double d2 = dr * dr + di * di;
            if (d2 < r.extra) r.extra = d2;
        }
        orbit_scratch.push_back(z);

        bool escaped_now = !finite_z;
        if constexpr (variant_is_transcendental_v<V>()) {
            const double component = std::max(std::fabs(zre), std::fabs(zim));
            escaped_now = escaped_now || component >= bailout;
        } else {
            escaped_now = escaped_now || n2 > bailout_sq;
        }
        if (escaped_now) break;
    }

    if (r.extra != std::numeric_limits<double>::infinity()) {
        r.extra = scalar_sqrt(r.extra);
    }
    result = r;
    return true;
}

template <int FRAC, FixedEscapeGate Gate, IterResultMask NeedMask, Variant V>
bool iterate_fixed_field_cancellable(
    Cx<Fixed64<FRAC>> z,
    const Cx<Fixed64<FRAC>>& c,
    int max_iter,
    uint64_t bailout_raw,
    uint64_t bailout2_raw,
    uint64_t two_raw,
    uint64_t two_sqrt2_floor_raw,
    bool precheck_initial_z0,
    const MapParams& p,
    std::atomic<bool>& cancelled,
    IterResult& result
) {
    static_assert(!iter_result_wants(NeedMask, IterResultField::Extra),
        "Use iterate_pairwise for MinPairwiseDist.");
    static_assert(!variant_is_transcendental_v<V>(),
        "Fixed-point integer iteration only supports quadratic variants.");

    using S = Fixed64<FRAC>;
    IterResult r = make_iter_result<NeedMask>();

    constexpr bool track_min = iter_result_wants(NeedMask, IterResultField::MinAbs);
    constexpr bool track_max = iter_result_wants(NeedMask, IterResultField::MaxAbs);
    constexpr bool wants_norm = iter_result_wants(NeedMask, IterResultField::Norm);
    constexpr bool can_gate_without_mag2 = !(track_min || track_max || wants_norm);

    uint64_t min_mag2_raw = std::numeric_limits<uint64_t>::max();
    uint64_t max_mag2_raw = 0;

    S x = z.re;
    S y = z.im;
    uint64_t x2_raw = 0;
    uint64_t y2_raw = 0;

    if (precheck_initial_z0) {
        bool escaped_initial = false;
        uint64_t mag2_raw = 0;
        uint64_t l1_raw = 0;
        bool have_mag2 = false;

        if constexpr (Gate != FixedEscapeGate::Direct) {
            const uint64_t ax = abs_i64_to_u64(x.raw);
            const uint64_t ay = abs_i64_to_u64(y.raw);
            escaped_initial = ax > bailout_raw || ay > bailout_raw;
            if constexpr (Gate == FixedEscapeGate::L1) {
                if (!escaped_initial) {
                    l1_raw = add_u64_sat(ax, ay);
                    escaped_initial = l1_raw > two_sqrt2_floor_raw;
                }
            }
        }

        if constexpr (Gate == FixedEscapeGate::Direct || track_min || track_max || wants_norm) {
            x2_raw = fixed_square_q_sat_raw_cpu<FRAC>(x.raw);
            y2_raw = fixed_square_q_sat_raw_cpu<FRAC>(y.raw);
            mag2_raw = add_u64_sat(x2_raw, y2_raw);
            have_mag2 = true;
            if constexpr (track_min) min_mag2_raw = std::min(min_mag2_raw, mag2_raw);
            if constexpr (track_max) max_mag2_raw = std::max(max_mag2_raw, mag2_raw);
        }

        if (!escaped_initial) {
            if (!have_mag2) {
                x2_raw = fixed_square_q_sat_raw_cpu<FRAC>(x.raw);
                y2_raw = fixed_square_q_sat_raw_cpu<FRAC>(y.raw);
                mag2_raw = add_u64_sat(x2_raw, y2_raw);
                have_mag2 = true;
            }
            if constexpr (Gate == FixedEscapeGate::L1) {
                escaped_initial = l1_raw > two_raw && mag2_raw > bailout2_raw;
            } else {
                escaped_initial = mag2_raw > bailout2_raw;
            }
        } else if (!have_mag2 && (track_min || track_max || wants_norm)) {
            x2_raw = fixed_square_q_sat_raw_cpu<FRAC>(x.raw);
            y2_raw = fixed_square_q_sat_raw_cpu<FRAC>(y.raw);
            mag2_raw = add_u64_sat(x2_raw, y2_raw);
            if constexpr (track_min) min_mag2_raw = std::min(min_mag2_raw, mag2_raw);
            if constexpr (track_max) max_mag2_raw = std::max(max_mag2_raw, mag2_raw);
        }

        if (escaped_initial) {
            finish_fixed_escape<NeedMask, FRAC>(r, 0, mag2_raw, min_mag2_raw, max_mag2_raw);
            result = r;
            return true;
        }
    }

    if (x2_raw == 0 && y2_raw == 0 && (x.raw != 0 || y.raw != 0)) {
        x2_raw = fixed_square_q_sat_raw_cpu<FRAC>(x.raw);
        y2_raw = fixed_square_q_sat_raw_cpu<FRAC>(y.raw);
    }
    S x2{u64_to_i64_sat(x2_raw)};
    S y2{u64_to_i64_sat(y2_raw)};

    for (int i = 0; i < max_iter; i++) {
        if ((i & (kProgressiveFieldCancelStride - 1)) == 0 &&
            mark_cancelled_if_requested(p, cancelled)) {
            return false;
        }

        S nx{};
        S ny{};
        step_cached<V, S>(x, y, x2, y2, c.re, c.im, nx, ny);

        bool escaped_now = false;
        uint64_t mag2_raw = 0;
        uint64_t nx2_raw = 0;
        uint64_t ny2_raw = 0;
        uint64_t l1_raw = 0;

        if constexpr (can_gate_without_mag2 && Gate != FixedEscapeGate::Direct) {
            const uint64_t ax = abs_i64_to_u64(nx.raw);
            const uint64_t ay = abs_i64_to_u64(ny.raw);
            escaped_now = ax > bailout_raw || ay > bailout_raw;
            if constexpr (Gate == FixedEscapeGate::L1) {
                if (!escaped_now) {
                    l1_raw = add_u64_sat(ax, ay);
                    escaped_now = l1_raw > two_sqrt2_floor_raw;
                }
            }
        }

        if (!escaped_now) {
            nx2_raw = fixed_square_q_sat_raw_cpu<FRAC>(nx.raw);
            ny2_raw = fixed_square_q_sat_raw_cpu<FRAC>(ny.raw);
            mag2_raw = add_u64_sat(nx2_raw, ny2_raw);

            if constexpr (track_min) min_mag2_raw = std::min(min_mag2_raw, mag2_raw);
            if constexpr (track_max) max_mag2_raw = std::max(max_mag2_raw, mag2_raw);
            if constexpr (can_gate_without_mag2 && Gate == FixedEscapeGate::L1) {
                escaped_now = l1_raw > two_raw && mag2_raw > bailout2_raw;
            } else {
                escaped_now = mag2_raw > bailout2_raw;
            }
        }

        if (escaped_now) {
            finish_fixed_escape<NeedMask, FRAC>(r, i, mag2_raw, min_mag2_raw, max_mag2_raw);
            result = r;
            return true;
        }

        x = nx;
        y = ny;
        x2 = S{u64_to_i64_sat(nx2_raw)};
        y2 = S{u64_to_i64_sat(ny2_raw)};
    }

    if constexpr (iter_result_wants(NeedMask, IterResultField::Iter))    r.iter = max_iter;
    if constexpr (iter_result_wants(NeedMask, IterResultField::Escaped)) r.escaped = false;
    if constexpr (track_min) r.min_abs = fixed_mag2_q_to_abs<FRAC>(min_mag2_raw);
    if constexpr (track_max) r.max_abs = fixed_mag2_q_to_abs<FRAC>(max_mag2_raw);
    result = r;
    return true;
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
    const double span_re = p.scale * map_viewport_aspect(p);
    if (!fixed_double_fits_raw<FRAC>(span_re)) return false;
    const int64_t scale_raw = fixed_round_to_raw_sat<FRAC>(p.scale);
    const __int128 span_im_raw = static_cast<__int128>(scale_raw);
    const __int128 span_re_raw = static_cast<__int128>(
        fixed_round_to_raw_sat<FRAC>(span_re));
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
    if (p.rotation_deg != 0.0) {
        plan.fx = false;
        plan.fixed_precision = FixedPrecision::None;
    }

    // Engine selection is centralized so benchmark decisions are not silently
    // overwritten by a second, hard-coded preference ladder in this plan. The
    // raw field carries norm values, so smooth coloring does not need the old
    // color-kernel fallback that `map_engine_supported` still enforces for
    // direct BGR callers.
    MapParams selection_params = p;
    if (field_output) selection_params.smooth = false;
    plan.engine = select_map_engine(selection_params, plan.fx);

    // smooth (BGR) needs per-pixel |z|² which the AVX-512/CUDA *colorize* paths don't track;
    // the field kernels always emit norm, so field renders never gate on smooth.
    plan.needs_norm = field_output ? false : p.smooth;
    // Trig variants need scalar std::cmath — skip AVX-512/AVX-2/CUDA for them.
    plan.scalar_fallback = variant_needs_scalar_fallback(p.variant);
    return plan;
}


// Field variant for custom formula — fills FieldOutput with escape metric data.
static MapStats render_custom_field_openmp(const MapParams& p, FieldOutput& fo) {
    fo.width  = p.width;
    fo.height = p.height;
    fo.metric = Metric::Escape;  // custom always uses escape metric for field

    CustomStepFn fn = p.custom_step_fn;

    const int W = p.width, H = p.height;
    const double aspect  = map_viewport_aspect(p);
    const double span_im = p.scale;
    const double span_re = p.scale * aspect;
    const double re_min  = p.center_re - span_re * 0.5;
    const double im_max  = p.center_im + span_im * 0.5;
    const double bail2   = p.bailout_sq;
    const double jre     = p.julia_re;
    const double jim     = p.julia_im;
    const bool has_rot = p.rotation_deg != 0.0;
    const double rot_rad = has_rot ? p.rotation_deg * M_PI / 180.0 : 0.0;
    const double cos_t = has_rot ? std::cos(rot_rad) : 1.0;
    const double sin_t = has_rot ? std::sin(rot_rad) : 0.0;
    const double pixel_step_x = span_re / static_cast<double>(W);
    const double pixel_step_y = span_im / static_cast<double>(H);
    const double half_w = static_cast<double>(W) * 0.5;
    const double half_h = static_cast<double>(H) * 0.5;
    const int thread_count = resolve_render_threads(p.render_threads);
    const int tile_size = p.on_field_tile_done ? 8 : 32;
    const int tiles_x = ceil_div(W, tile_size);
    const int tiles_y = ceil_div(H, tile_size);
    const int tile_count = tiles_x * tiles_y;
    const std::vector<int> progressive_tile_order = p.on_field_tile_done
        ? map_field_progressive_order(tiles_x, tiles_y, true)
        : std::vector<int>{};
    std::atomic<bool> cancelled{false};

    fo.iter_u32.assign(static_cast<size_t>(W) * H, 0u);
    fo.norm_f32.assign(static_cast<size_t>(W) * H, 0.0f);

    const auto t0 = std::chrono::steady_clock::now();

    #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 1)
    for (int work_index = 0; work_index < tile_count; work_index++) {
        const int tile = progressive_tile_order.empty() ? work_index : progressive_tile_order[work_index];
        if (cancelled.load(std::memory_order_relaxed)) continue;
        if (map_render_cancel_requested(p)) {
            cancelled.store(true, std::memory_order_relaxed);
            continue;
        }
        const int tile_x = tile % tiles_x;
        const int tile_y = tile / tiles_x;
        const int x0 = tile_x * tile_size;
        const int y0 = tile_y * tile_size;
        const int x1 = std::min(W, x0 + tile_size);
        const int y1 = std::min(H, y0 + tile_size);

        bool tile_cancelled = false;
        for (int y = y0; y < y1; y++) {
            if (cancelled.load(std::memory_order_relaxed) || map_render_cancel_requested(p)) {
                cancelled.store(true, std::memory_order_relaxed);
                tile_cancelled = true;
                break;
            }
            const double dy_base = -(static_cast<double>(y) + 0.5 - half_h) * pixel_step_y;
            const double im_norot = im_max - (static_cast<double>(y) + 0.5) / H * span_im;
            for (int x = x0; x < x1; x++) {
                if (cancelled.load(std::memory_order_relaxed) || map_render_cancel_requested(p)) {
                    cancelled.store(true, std::memory_order_relaxed);
                    tile_cancelled = true;
                    break;
                }
                double re_c, im_c;
                if (has_rot) {
                    const double dx = (static_cast<double>(x) + 0.5 - half_w) * pixel_step_x;
                    re_c = p.center_re + dx * cos_t - dy_base * sin_t;
                    im_c = p.center_im + dx * sin_t + dy_base * cos_t;
                } else {
                    re_c = re_min + (static_cast<double>(x) + 0.5) / W * span_re;
                    im_c = im_norot;
                }

                double zr, zi, cr, ci;
                if (p.julia) { zr = re_c; zi = im_c; cr = jre; ci = jim; }
                else          { zr = 0.0; zi = 0.0;  cr = re_c; ci = im_c; }

                int   it    = 0;
                double norm2 = 0.0;
                for (; it < p.iterations; it++) {
                    if ((it & 511) == 0 && map_render_cancel_requested(p)) {
                        cancelled.store(true, std::memory_order_relaxed);
                        tile_cancelled = true;
                        break;
                    }
                    double nr = 0.0, ni = 0.0;
                    fn(zr, zi, cr, ci, &nr, &ni);
                    zr = nr; zi = ni;
                    const bool finite_z = std::isfinite(zr) && std::isfinite(zi);
                    norm2 = finite_z ? (zr * zr + zi * zi)
                                     : std::numeric_limits<double>::infinity();
                    if (!finite_z || norm2 > bail2) break;
                }
                if (tile_cancelled) break;

                const bool escaped = (norm2 > bail2);
                const size_t idx = static_cast<size_t>(y) * W + x;
                fo.iter_u32[idx] = escaped ? static_cast<uint32_t>(it) : static_cast<uint32_t>(p.iterations);
                fo.norm_f32[idx] = escaped ? static_cast<float>(norm2) : 0.0f;
            }
            if (tile_cancelled) break;
        }
        if (!tile_cancelled && p.on_field_tile_done) p.on_field_tile_done(x0, y0, x1 - x0, y1 - y0);
    }

    if (cancelled.load(std::memory_order_relaxed) || map_render_cancel_requested(p)) {
        throw_render_cancelled();
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

// Safe Orbit Program field renderer. A finite bailout is consulted only when
// the program carries a proof certificate. Unverified programs deliberately
// run to max iterations unless arithmetic becomes non-finite; numerical
// divergence is recorded separately and is never encoded as escape.
static MapStats render_orbit_field_openmp(const MapParams& p, FieldOutput& fo) {
    if (!p.orbit_program) throw std::runtime_error("missing Orbit Program");
    if (p.metric != Metric::Escape) {
        throw std::runtime_error("Orbit Program v1 supports only the escape metric");
    }
    fo.width = p.width;
    fo.height = p.height;
    fo.metric = Metric::Escape;

    const int W = p.width;
    const int H = p.height;
    const double aspect = map_viewport_aspect(p);
    const double span_im = p.scale;
    const double span_re = p.scale * aspect;
    const double re_min = p.center_re - span_re * 0.5;
    const double im_max = p.center_im + span_im * 0.5;
    const double jre = p.julia_re;
    const double jim = p.julia_im;
    const bool has_rot = p.rotation_deg != 0.0;
    const double rot_rad = has_rot ? p.rotation_deg * M_PI / 180.0 : 0.0;
    const double cos_t = has_rot ? std::cos(rot_rad) : 1.0;
    const double sin_t = has_rot ? std::sin(rot_rad) : 0.0;
    const double pixel_step_x = span_re / static_cast<double>(W);
    const double pixel_step_y = span_im / static_cast<double>(H);
    const double half_w = static_cast<double>(W) * 0.5;
    const double half_h = static_cast<double>(H) * 0.5;
    const EscapeAnalysis& analysis = p.orbit_program->escape_analysis();
    const bool certified = analysis.has_finite_radius();
    const int thread_count = resolve_render_threads(p.render_threads);
    const int tile_size = p.on_field_tile_done ? 8 : 32;
    const int tiles_x = ceil_div(W, tile_size);
    const int tiles_y = ceil_div(H, tile_size);
    const int tile_count = tiles_x * tiles_y;
    const std::vector<int> progressive_tile_order = p.on_field_tile_done
        ? map_field_progressive_order(tiles_x, tiles_y, true)
        : std::vector<int>{};
    std::atomic<bool> cancelled{false};

    const size_t pixel_count = static_cast<size_t>(W) * H;
    fo.iter_u32.assign(pixel_count, static_cast<uint32_t>(p.iterations));
    fo.norm_f32.assign(pixel_count, 0.0f);
    fo.orbit_class_u8.assign(pixel_count, 0u);
    const auto t0 = std::chrono::steady_clock::now();

    #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 1)
    for (int work_index = 0; work_index < tile_count; ++work_index) {
        const int tile = progressive_tile_order.empty() ? work_index : progressive_tile_order[work_index];
        if (cancelled.load(std::memory_order_relaxed)) continue;
        if (map_render_cancel_requested(p)) {
            cancelled.store(true, std::memory_order_relaxed);
            continue;
        }
        const int tile_x = tile % tiles_x;
        const int tile_y = tile / tiles_x;
        const int x0 = tile_x * tile_size;
        const int y0 = tile_y * tile_size;
        const int x1 = std::min(W, x0 + tile_size);
        const int y1 = std::min(H, y0 + tile_size);
        bool tile_cancelled = false;

        for (int y = y0; y < y1 && !tile_cancelled; ++y) {
            const double dy_base = -(static_cast<double>(y) + 0.5 - half_h) * pixel_step_y;
            const double im_norot = im_max - (static_cast<double>(y) + 0.5) / H * span_im;
            for (int x = x0; x < x1; ++x) {
                if (cancelled.load(std::memory_order_relaxed) || map_render_cancel_requested(p)) {
                    cancelled.store(true, std::memory_order_relaxed);
                    tile_cancelled = true;
                    break;
                }
                double pixel_re;
                double pixel_im;
                if (has_rot) {
                    const double dx = (static_cast<double>(x) + 0.5 - half_w) * pixel_step_x;
                    pixel_re = p.center_re + dx * cos_t - dy_base * sin_t;
                    pixel_im = p.center_im + dx * sin_t + dy_base * cos_t;
                } else {
                    pixel_re = re_min + (static_cast<double>(x) + 0.5) / W * span_re;
                    pixel_im = im_norot;
                }

                Cx<double> z = p.julia ? Cx<double>{pixel_re, pixel_im} : Cx<double>{0.0, 0.0};
                const Cx<double> c = p.julia ? Cx<double>{jre, jim} : Cx<double>{pixel_re, pixel_im};
                // For Julia, c is independent of the seed, so use the
                // sufficient quadratic bound R^2-|c|>R. For Mandelbrot-family
                // parameter maps starting at zero, the certified template's
                // radius-2 first-crossing proof applies directly.
                double certified_bailout_sq = std::numeric_limits<double>::infinity();
                if (certified) {
                    double radius = analysis.certified_radius;
                    if (p.julia) {
                        const double c_abs = std::hypot(c.re, c.im);
                        const double julia_radius = 0.5 * (1.0 + std::sqrt(1.0 + 4.0 * c_abs));
                        radius = std::max(radius, julia_radius);
                    }
                    certified_bailout_sq = radius * radius;
                }
                bool escaped = false;
                bool numerical_divergence = false;
                double norm2 = 0.0;
                int iteration = 0;
                for (; iteration < p.iterations; ++iteration) {
                    if ((iteration & 511) == 0 && map_render_cancel_requested(p)) {
                        cancelled.store(true, std::memory_order_relaxed);
                        tile_cancelled = true;
                        break;
                    }
                    z = p.orbit_program->step(z, c, iteration);
                    if (!std::isfinite(z.re) || !std::isfinite(z.im)) {
                        numerical_divergence = true;
                        break;
                    }
                    norm2 = z.re * z.re + z.im * z.im;
                    if (!std::isfinite(norm2)) {
                        numerical_divergence = true;
                        break;
                    }
                    if (certified && norm2 > certified_bailout_sq) {
                        escaped = true;
                        break;
                    }
                }
                if (tile_cancelled) break;
                const size_t index = static_cast<size_t>(y) * W + x;
                if (escaped) {
                    fo.iter_u32[index] = static_cast<uint32_t>(iteration);
                    fo.norm_f32[index] = static_cast<float>(norm2);
                    fo.orbit_class_u8[index] = 1u;
                } else if (numerical_divergence) {
                    fo.orbit_class_u8[index] = 2u;
                }
            }
        }
        if (!tile_cancelled && p.on_field_tile_done) {
            p.on_field_tile_done(x0, y0, x1 - x0, y1 - y0);
        }
    }
    if (cancelled.load(std::memory_order_relaxed) || map_render_cancel_requested(p)) {
        throw_render_cancelled();
    }

    const auto t1 = std::chrono::steady_clock::now();
    MapStats stats;
    stats.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    stats.pixel_count = W * H;
    stats.scalar_used = "fp64";
    stats.engine_used = "openmp";
    fo.elapsed_ms = stats.elapsed_ms;
    fo.scalar_used = stats.scalar_used;
    fo.engine_used = stats.engine_used;
    return stats;
}

// ─── Raw field renderer (always OpenMP, no colorization) ─────────────────────

namespace {

template <Variant V, typename S, Metric M, IterResultMask NeedMask>
void field_variant_impl(const MapParams& p, FieldOutput& out) {
    const int W = p.width;
    const int H = p.height;
    const double aspect  = map_viewport_aspect(p);
    const double span_im = p.scale;
    const double span_re = p.scale * aspect;
    const double bail2   = p.bailout_sq;

    const S s_center_re = scalar_from_string<S>(p.center_re_str, p.center_re);
    const S s_center_im = scalar_from_string<S>(p.center_im_str, p.center_im);
    const S s_span_re   = scalar_from_double<S>(span_re);
    const S s_span_im   = scalar_from_double<S>(span_im);
    const S s_half      = scalar_from_double<S>(0.5);
    const S s_re_min    = s_center_re - s_span_re * s_half;
    const S s_im_max    = s_center_im + s_span_im * s_half;
    const S s_inv_W     = scalar_from_double<S>(1.0 / W);
    const S s_inv_H     = scalar_from_double<S>(1.0 / H);

    const bool has_rot = p.rotation_deg != 0.0;
    const double rot_rad = has_rot ? p.rotation_deg * M_PI / 180.0 : 0.0;
    const S s_cos_t = scalar_from_double<S>(has_rot ? std::cos(rot_rad) : 1.0);
    const S s_sin_t = scalar_from_double<S>(has_rot ? std::sin(rot_rad) : 0.0);
    const S s_pixel_step_x = scalar_from_double<S>(span_re / static_cast<double>(W));
    const S s_pixel_step_y = scalar_from_double<S>(span_im / static_cast<double>(H));
    const S s_half_w     = scalar_from_double<S>(static_cast<double>(W) * 0.5);
    const S s_half_h     = scalar_from_double<S>(static_cast<double>(H) * 0.5);

    const S jre = scalar_from_double<S>(p.julia_re);
    const S jim = scalar_from_double<S>(p.julia_im);
    const Cx<S> c_const{jre, jim};

    const int thread_count = resolve_render_threads(p.render_threads);
    constexpr bool is_escape = (M == Metric::Escape);
    const int tile_size = p.on_field_tile_done ? 8 : 32;
    const int tiles_x = ceil_div(W, tile_size);
    const int tiles_y = ceil_div(H, tile_size);
    const int tile_count = tiles_x * tiles_y;
    const std::vector<int> progressive_tile_order = p.on_field_tile_done
        ? map_field_progressive_order(tiles_x, tiles_y, true)
        : std::vector<int>{};
    const bool cancellable_progressive = static_cast<bool>(p.on_field_tile_done);

    if constexpr (is_escape) {
        out.iter_u32.assign(static_cast<size_t>(W) * H, 0u);
        out.norm_f32.assign(static_cast<size_t>(W) * H, 0.0f);
    } else {
        out.field_f64.assign(static_cast<size_t>(W) * H, 0.0);
    }

    std::atomic<bool> cancelled{false};

    #pragma omp parallel num_threads(thread_count)
    {
        std::vector<Cx<S>> orbit;
        if constexpr (M == Metric::MinPairwiseDist) {
            orbit.reserve(static_cast<size_t>(p.pairwise_cap));
        }

        #pragma omp for schedule(dynamic, 1)
        for (int work_index = 0; work_index < tile_count; work_index++) {
            const int tile = progressive_tile_order.empty() ? work_index : progressive_tile_order[work_index];
            if (cancelled.load(std::memory_order_relaxed)) continue;
            if (map_render_cancel_requested(p)) {
                cancelled.store(true, std::memory_order_relaxed);
                continue;
            }
            const int tile_x = tile % tiles_x;
            const int tile_y = tile / tiles_x;
            const int x0 = tile_x * tile_size;
            const int y0 = tile_y * tile_size;
            const int x1 = std::min(W, x0 + tile_size);
            const int y1 = std::min(H, y0 + tile_size);

            bool tile_cancelled = false;
            for (int y = y0; y < y1; y++) {
                if (cancelled.load(std::memory_order_relaxed) || map_render_cancel_requested(p)) {
                    cancelled.store(true, std::memory_order_relaxed);
                    tile_cancelled = true;
                    break;
                }
                const S im_norot = s_im_max - scalar_from_double<S>(static_cast<double>(y) + 0.5) * s_inv_H * s_span_im;
                const S dy_base = -(scalar_from_double<S>(static_cast<double>(y) + 0.5) - s_half_h) * s_pixel_step_y;
                for (int x = x0; x < x1; x++) {
                    if (cancellable_progressive && mark_cancelled_if_requested(p, cancelled)) {
                        tile_cancelled = true;
                        break;
                    }
                    S re, im;
                    if (has_rot) {
                        const S dx = (scalar_from_double<S>(static_cast<double>(x) + 0.5) - s_half_w) * s_pixel_step_x;
                        re = s_center_re + dx * s_cos_t - dy_base * s_sin_t;
                        im = s_center_im + dx * s_sin_t + dy_base * s_cos_t;
                    } else {
                        re = s_re_min + scalar_from_double<S>(static_cast<double>(x) + 0.5) * s_inv_W * s_span_re;
                        im = im_norot;
                    }

                    Cx<S> z0, c;
                    if (p.julia) {
                        z0 = Cx<S>{re, im};
                        c  = c_const;
                    } else {
                        z0 = Cx<S>{scalar_from_double<S>(0.0), scalar_from_double<S>(0.0)};
                        c  = Cx<S>{re, im};
                    }

                    IterResult r;
                    bool pixel_complete = true;
                    if constexpr (M == Metric::MinPairwiseDist) {
                        if (cancellable_progressive) {
                            pixel_complete = iterate_pairwise_field_cancellable<V, S>(
                                z0, c, p.iterations, p.bailout, bail2, p.pairwise_cap,
                                orbit, p, cancelled, r);
                        } else {
                            r = iterate_pairwise<V, S>(
                                z0, c, p.iterations, p.bailout, bail2, p.pairwise_cap, orbit);
                        }
                    } else {
                        if (cancellable_progressive) {
                            pixel_complete = iterate_field_cancellable<NeedMask, V, S>(
                                z0, c, p.iterations, p.bailout, bail2, p, cancelled, r);
                        } else {
                            r = iterate_masked<NeedMask, V, S>(
                                z0, c, p.iterations, p.bailout, bail2);
                        }
                    }
                    if (!pixel_complete) {
                        tile_cancelled = true;
                        break;
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
                if (tile_cancelled) break;
            }
            if (!tile_cancelled && p.on_field_tile_done) p.on_field_tile_done(x0, y0, x1 - x0, y1 - y0);
        }
    }

    throw_if_cancelled(p, cancelled);

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
        p.center_re, p.center_im, p.scale, map_viewport_aspect(p), W, H,
        p.julia_re, p.julia_im, p.bailout, p.bailout_sq);

    const int thread_count = resolve_render_threads(p.render_threads);
    constexpr bool is_escape = (M == Metric::Escape);
    const int tile_size = p.on_field_tile_done ? 8 : 32;
    const int tiles_x = ceil_div(W, tile_size);
    const int tiles_y = ceil_div(H, tile_size);
    const int tile_count = tiles_x * tiles_y;
    const std::vector<int> progressive_tile_order = p.on_field_tile_done
        ? map_field_progressive_order(tiles_x, tiles_y, true)
        : std::vector<int>{};
    const bool cancellable_progressive = static_cast<bool>(p.on_field_tile_done);

    if constexpr (is_escape) {
        out.iter_u32.assign(static_cast<size_t>(W) * H, 0u);
        out.norm_f32.assign(static_cast<size_t>(W) * H, 0.0f);
    } else {
        out.field_f64.assign(static_cast<size_t>(W) * H, 0.0);
    }

    const Cx<S> c_const{S{vp.julia_re_raw}, S{vp.julia_im_raw}};
    std::atomic<bool> cancelled{false};

    #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 1)
    for (int work_index = 0; work_index < tile_count; work_index++) {
        const int tile = progressive_tile_order.empty() ? work_index : progressive_tile_order[work_index];
        if (cancelled.load(std::memory_order_relaxed)) continue;
        if (map_render_cancel_requested(p)) {
            cancelled.store(true, std::memory_order_relaxed);
            continue;
        }
        const int tile_x = tile % tiles_x;
        const int tile_y = tile / tiles_x;
        const int x0 = tile_x * tile_size;
        const int y0 = tile_y * tile_size;
        const int x1 = std::min(W, x0 + tile_size);
        const int y1 = std::min(H, y0 + tile_size);

        bool tile_cancelled = false;
        for (int y = y0; y < y1; y++) {
            if (cancelled.load(std::memory_order_relaxed) || map_render_cancel_requested(p)) {
                cancelled.store(true, std::memory_order_relaxed);
                tile_cancelled = true;
                break;
            }
            const S im{fixed_pixel_im_raw<FRAC>(vp, y)};
            for (int x = x0; x < x1; x++) {
                if (cancellable_progressive && mark_cancelled_if_requested(p, cancelled)) {
                    tile_cancelled = true;
                    break;
                }
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
                IterResult r;
                bool pixel_complete = true;
                if (cancellable_progressive) {
                    pixel_complete = iterate_fixed_field_cancellable<FRAC, gate, NeedMask, V>(
                        z0, c, p.iterations, vp.bailout_raw, vp.bailout2_raw,
                        vp.two_raw, vp.two_sqrt2_floor_raw, p.julia, p, cancelled, r);
                } else {
                    r = iterate_fixed_int_masked<FRAC, gate, NeedMask, V>(
                        z0, c, p.iterations, vp.bailout_raw, vp.bailout2_raw,
                        vp.two_raw, vp.two_sqrt2_floor_raw, p.julia);
                }
                if (!pixel_complete) {
                    tile_cancelled = true;
                    break;
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
            if (tile_cancelled) break;
        }
        if (!tile_cancelled && p.on_field_tile_done) p.on_field_tile_done(x0, y0, x1 - x0, y1 - y0);
    }

    throw_if_cancelled(p, cancelled);

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
        case Metric::MandelShipAgree:
            break;  // Handled by render_explore_field() before scalar dispatch.
    }
}

template <Variant V>
void field_variant_fp32(const MapParams& p, FieldOutput& out) {
    field_variant_scalar<V, float>(p, out);
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
        case Metric::MandelShipAgree:
            break;  // Handled by render_explore_field() before fixed dispatch.
    }
}

void dispatch_field_fp32(const MapParams& p, FieldOutput& out) {
    switch (p.variant) {
        case Variant::Mandelbrot: field_variant_fp32<Variant::Mandelbrot>(p, out); break;
        case Variant::Tri:        field_variant_fp32<Variant::Tri>       (p, out); break;
        case Variant::Boat:       field_variant_fp32<Variant::Boat>      (p, out); break;
        case Variant::Duck:       field_variant_fp32<Variant::Duck>      (p, out); break;
        case Variant::Bell:       field_variant_fp32<Variant::Bell>      (p, out); break;
        case Variant::Fish:       field_variant_fp32<Variant::Fish>      (p, out); break;
        case Variant::Vase:       field_variant_fp32<Variant::Vase>      (p, out); break;
        case Variant::Bird:       field_variant_fp32<Variant::Bird>      (p, out); break;
        case Variant::Mask:       field_variant_fp32<Variant::Mask>      (p, out); break;
        case Variant::Ship:       field_variant_fp32<Variant::Ship>      (p, out); break;
        case Variant::SinZ:       field_variant_fp32<Variant::SinZ>      (p, out); break;
        case Variant::CosZ:       field_variant_fp32<Variant::CosZ>      (p, out); break;
        case Variant::ExpZ:       field_variant_fp32<Variant::ExpZ>      (p, out); break;
        case Variant::SinhZ:      field_variant_fp32<Variant::SinhZ>     (p, out); break;
        case Variant::CoshZ:      field_variant_fp32<Variant::CoshZ>     (p, out); break;
        case Variant::TanZ:       field_variant_fp32<Variant::TanZ>      (p, out); break;
        default: break;  // Variant::Custom intercepted before this dispatch
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

// ── Guided exploration: variant vs Mandelbrot ────────────────────────────────
// Every polynomial variant is the Mandelbrot step z²+c with an extra fold (abs / negate /
// conjugate of some part), so its orbit coincides with the plain Mandelbrot's until the first
// iterate where that fold actually changes the result. This mode renders the *selected* variant
// and inverts the pixels whose orbit ever diverges from the Mandelbrot — the structure unique to
// the variant stands out, while the regions it
// reproduces from the Mandelbrot keep their colour. Generic over all variants (Boat = Burning
// Ship, plus Celtic, Buffalo, Heart, … and the trig variants, which simply differ everywhere).
// Scalar-OpenMP — an analysis overlay, not a hot path.
namespace {

// Run variant V's orbit; report the escape iteration and whether it stayed bit-identical to the
// Mandelbrot orbit the whole way. The two share every step until the first where
// variant_step<V>(z,c) != variant_step<Mandelbrot>(z,c) — i.e. the fold first alters the result.
template <Variant V>
inline void variant_explore_orbit(Cx<double> z, const Cx<double>& c, int max_iter, double bail2,
                                  int& out_iter, bool& out_fully) {
    bool diverged = false;
    int i = 0;
    for (; i < max_iter; ++i) {
        const Cx<double> vn = variant_step<V, double>(z, c);
        if (!diverged) {
            const Cx<double> mn = variant_step<Variant::Mandelbrot, double>(z, c);
            // A no-op fold gives a mathematically exact match; a real fold changes a term by a
            // non-trivial amount. Use a small relative tolerance so 1-ULP FMA/contraction noise
            // between the two step computations doesn't read as divergence (it otherwise marks
            // even Mandelbrot-vs-itself on ~a quarter of pixels).
            const double tol = 1e-11 * (1.0 + std::fabs(mn.re) + std::fabs(mn.im));
            if (std::fabs(vn.re - mn.re) > tol || std::fabs(vn.im - mn.im) > tol) diverged = true;
        }
        z = vn;
        if (z.re * z.re + z.im * z.im > bail2) break;   // escaped (i = escape iteration)
    }
    out_iter = i;
    out_fully = !diverged;
}

// Walk every pixel of variant V's explore orbit; write(y, x, iter, fully) does the output.
template <Variant V, class Write>
void explore_pixels(const MapParams& p, Write&& write) {
    const int W = p.width, H = p.height;
    const double aspect = map_viewport_aspect(p);
    const double span_im = p.scale, span_re = p.scale * aspect;
    const double re_min = p.center_re - span_re * 0.5;
    const double im_max = p.center_im + span_im * 0.5;
    const double bail2 = p.bailout_sq;
    const int max_iter = p.iterations;
    const int thread_count = resolve_render_threads(p.render_threads);
    #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 8)
    for (int y = 0; y < H; ++y) {
        if (map_render_cancel_requested(p)) continue;
        const double im = im_max - (static_cast<double>(y) + 0.5) / H * span_im;
        for (int x = 0; x < W; ++x) {
            const double re = re_min + (static_cast<double>(x) + 0.5) / W * span_re;
            Cx<double> z, c;
            if (p.julia) { z = Cx<double>{re, im};   c = Cx<double>{p.julia_re, p.julia_im}; }
            else         { z = Cx<double>{0.0, 0.0}; c = Cx<double>{re, im}; }
            int it; bool fully;
            variant_explore_orbit<V>(z, c, max_iter, bail2, it, fully);
            write(y, x, it, fully);
        }
    }
}

template <Variant V>
MapStats render_variant_explore_mat(const MapParams& p, cv::Mat& out) {
    if (out.empty() || out.rows != p.height || out.cols != p.width || out.type() != CV_8UC3) {
        out.create(p.height, p.width, CV_8UC3);
    }
    const auto t0 = std::chrono::steady_clock::now();
    explore_pixels<V>(p, [&](int y, int x, int it, bool fully) {
        uint8_t b, g, r;
        colorize_escape_bgr(it, p.iterations, p.colormap, 0.0, false, b, g, r);
        uint8_t* px = out.ptr<uint8_t>(y) + 3 * x;
        if (fully) {
            px[0] = b;
            px[1] = g;
            px[2] = r;
        } else {
            px[0] = static_cast<uint8_t>(255 - static_cast<int>(b));
            px[1] = static_cast<uint8_t>(255 - static_cast<int>(g));
            px[2] = static_cast<uint8_t>(255 - static_cast<int>(r));
        }
    });
    if (map_render_cancel_requested(p)) throw_render_cancelled();
    const auto t1 = std::chrono::steady_clock::now();
    MapStats s;
    s.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    s.pixel_count = p.width * p.height;
    s.scalar_used = "fp64";
    s.engine_used = "openmp";
    return s;
}

template <Variant V>
MapStats render_variant_explore_field(const MapParams& p, FieldOutput& fo) {
    fo.width = p.width;
    fo.height = p.height;
    fo.metric = Metric::MandelShipAgree;
    fo.field_f64.assign(static_cast<size_t>(p.width) * static_cast<size_t>(p.height), 0.0);
    const auto t0 = std::chrono::steady_clock::now();
    explore_pixels<V>(p, [&](int y, int x, int, bool fully) {
        fo.field_f64[static_cast<size_t>(y) * static_cast<size_t>(p.width) + x] = fully ? 1.0 : 0.0;
    });
    fo.field_min = 0.0;
    fo.field_max = 1.0;
    const auto t1 = std::chrono::steady_clock::now();
    MapStats s;
    s.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    s.pixel_count = p.width * p.height;
    s.scalar_used = "fp64";
    s.engine_used = "openmp";
    fo.scalar_used = "fp64";
    fo.engine_used = "openmp";
    return s;
}

} // anonymous namespace

static MapStats render_explore(const MapParams& p, cv::Mat& out) {
#define FSD_EXPLORE(VV) case Variant::VV: return render_variant_explore_mat<Variant::VV>(p, out);
    switch (p.variant) {
        FSD_EXPLORE(Mandelbrot) FSD_EXPLORE(Tri)   FSD_EXPLORE(Boat)  FSD_EXPLORE(Duck)
        FSD_EXPLORE(Bell)       FSD_EXPLORE(Fish)  FSD_EXPLORE(Vase)  FSD_EXPLORE(Bird)
        FSD_EXPLORE(Mask)       FSD_EXPLORE(Ship)  FSD_EXPLORE(SinZ)  FSD_EXPLORE(CosZ)
        FSD_EXPLORE(ExpZ)       FSD_EXPLORE(SinhZ) FSD_EXPLORE(CoshZ) FSD_EXPLORE(TanZ)
        default: return render_variant_explore_mat<Variant::Boat>(p, out);
    }
#undef FSD_EXPLORE
}


static MapStats render_explore_field(const MapParams& p, FieldOutput& fo) {
#define FSD_EXPLORE_F(VV) case Variant::VV: return render_variant_explore_field<Variant::VV>(p, fo);
    switch (p.variant) {
        FSD_EXPLORE_F(Mandelbrot) FSD_EXPLORE_F(Tri)   FSD_EXPLORE_F(Boat)  FSD_EXPLORE_F(Duck)
        FSD_EXPLORE_F(Bell)       FSD_EXPLORE_F(Fish)  FSD_EXPLORE_F(Vase)  FSD_EXPLORE_F(Bird)
        FSD_EXPLORE_F(Mask)       FSD_EXPLORE_F(Ship)  FSD_EXPLORE_F(SinZ)  FSD_EXPLORE_F(CosZ)
        FSD_EXPLORE_F(ExpZ)       FSD_EXPLORE_F(SinhZ) FSD_EXPLORE_F(CoshZ) FSD_EXPLORE_F(TanZ)
        default: return render_variant_explore_field<Variant::Boat>(p, fo);
    }
#undef FSD_EXPLORE_F
}

MapStats render_map_field(const MapParams& p, FieldOutput& fo) {
    if (p.orbit_program) {
        return render_orbit_field_openmp(p, fo);
    }
    // Custom variant: use function-pointer path (always OpenMP).
    if (p.variant == Variant::Custom && p.custom_step_fn) {
        return render_custom_field_openmp(p, fo);
    }
    if (p.metric == Metric::MandelShipAgree) {
        return render_explore_field(p, fo);
    }

    // Perturbation field path: deep-zoom Mandelbrot/Julia for every metric it
    // can serve (Escape incl. smooth/equalized coloring, MinAbs, MaxAbs,
    // Envelope — MinPairwiseDist needs the whole orbit point set per pixel).
    // Only activate below fp64's useful range (~1e-13) so the fast SIMD/CUDA
    // fp64 field path handles moderate deep zoom.
    if (perturbation_applicable(p)) {
        return render_map_field_perturbation(p, fo);
    }

    fo.width  = p.width;
    fo.height = p.height;
    fo.metric = p.metric;

    // ── Fast SIMD/CUDA field path ────────────────────────────────────────────────
    // The hot consumer (equalized coloring) reads iter_u32; only the escape metric
    // carries per-pixel counts. Mirror render_map's engine choice. Everything else —
    // non-escape metrics, fp80/fp128, fixed-point (no SIMD field yet), custom/trig, or an
    // explicit "openmp" engine — falls through to the scalar dispatch below. Engines are
    // tried fastest-first; a stub/unavailable backend is skipped via its availability gate.
    MapEnginePlan field_plan = select_map_engine_plan(p, /*field_output=*/true);
    // A progressive field consumer needs published CPU-visible row/tile
    // boundaries. CUDA and the current hybrid field path only expose a whole
    // frame (or use a separate tiled viewport), so keep the same native
    // geometry on an incremental SIMD/OpenMP path instead.
    if (p.on_field_tile_done && (field_plan.engine == "cuda" || field_plan.engine == "hybrid")) {
        if (!field_plan.fp32 && avx512_available()) field_plan.engine = "avx512";
        else if (avx2_available() && fma_available()) field_plan.engine = "avx2";
        else field_plan.engine = "openmp";
    }
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
        if (field_plan.engine == "hybrid") {
            auto ts = render_map_field_hybrid(p, fo);
            MapStats s;
            s.elapsed_ms  = ts.total_ms;
            s.pixel_count = p.width * p.height;
            s.scalar_used = ts.scalar_used;
            s.engine_used = ts.engine_used;
            fo.scalar_used = s.scalar_used;
            fo.engine_used = s.engine_used;
            return s;
        }
#if USE_CUDA
        // Honor an explicitly/benchmark-selected CUDA field (fp32/fp64). On failure or a
        // non-cuda result, fall through to the CPU SIMD paths below.
        if (field_plan.engine == "cuda" && fsd_cuda::cuda_available()) {
            try {
                // Cheap launches finish before tiled scheduling pays for
                // itself. Only potentially long cancellable work uses the
                // existing tiled CPU/GPU scheduler.
                if (p.should_cancel && p.iterations > 2048) {
                    auto ts = render_map_field_hybrid(p, fo, 64);
                    MapStats s;
                    s.elapsed_ms = ts.total_ms;
                    s.pixel_count = p.width * p.height;
                    s.scalar_used = ts.scalar_used;
                    s.engine_used = ts.engine_used;
                    fo.scalar_used = s.scalar_used;
                    fo.engine_used = s.engine_used;
                    return s;
                }
                fsd_cuda::CudaMapParams cp;
                cp.center_re = p.center_re;
                cp.center_im = p.center_im;
                cp.scale     = p.scale;
                cp.viewport_aspect = map_viewport_aspect(p);
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
                cp.rotation_deg = p.rotation_deg;
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
                if (map_render_cancel_requested(p)) throw;
                // CUDA launch/runtime failure → CPU fallback below.
            }
        }
#endif
        // Honor an explicitly-selected AVX-2 (e.g. AVX-512-less CPUs, path-diff coverage).
        if (field_plan.engine == "avx2" && avx2_available() && fma_available()) {
            auto s = render_map_avx2_field(p, fo);
            if (accept(s, "avx2")) return s;
        }
        // Default fp64 fast field: AVX-512 (covers avx512 / hybrid / auto selections).
        // The AVX-512 field kernel is fp64-only; fp32 continues to the AVX2 field
        // kernel below instead of silently reporting fp64 for an fp32 request.
        if (!field_plan.fp32 && avx512_available()) {
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
    const bool fp32 = effective_scalar == "fp32";
    const bool fp80 = effective_scalar == "fp80";
    const bool fp128 = effective_scalar == "fp128";
    const FixedPrecision fixed_precision = select_fixed_precision(p);
    const bool fx = fixed_precision != FixedPrecision::None &&
                    supports_fixed_int_path(p, true);
    const auto t0 = std::chrono::steady_clock::now();

    if (fp32) {
        dispatch_field_fp32(p, fo);
    } else if (fx && fixed_precision == FixedPrecision::Q360) {
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
    s.scalar_used = fp32 ? "fp32"
                  : (fx ? fixed_precision_name(fixed_precision)
                  : (fp80 ? "fp80" : (fp128 ? "fp128" : "fp64")));
    s.engine_used = "openmp";
    fo.scalar_used = s.scalar_used;
    fo.engine_used = s.engine_used;
    return s;
}

// ─── Colorized map renderer ───────────────────────────────────────────────────
// Delegates to render_map_field() for numeric computation, then colorize_direct()
// for BGR output. All engine selection (CUDA / AVX-512 / AVX2 / hybrid / OpenMP)
// happens inside render_map_field(); this function is now a thin wrapper.

MapStats render_map(const MapParams& p, cv::Mat& out) {
    if (map_render_cancel_requested(p)) throw_render_cancelled();
    if (p.metric == Metric::MandelShipAgree) {
        return render_explore(p, out);
    }
    FieldOutput fo;
    auto stats = render_map_field(p, fo);
    out = colorize_direct(p, fo);
    return stats;
}

} // namespace fsd::compute
