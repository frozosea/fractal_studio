// compute/escape_time.hpp
//
// Per-pixel iteration core. Given a point `c` (or for Julia, a fixed `c` and
// a variable starting `z`), iterate the selected variant up to `max_iter`
// steps or until escape, and accumulate the requested metric.
//
// Metrics:
//   Escape          classic escape-time (iteration index at which |z|>2)
//   MinAbs          min_{n≤N} |z_n|              — HS-Base field
//   MaxAbs          max_{n≤N} |z_n|
//   Envelope        returns both MinAbs and MaxAbs packed into one sample
//   MinPairwiseDist min_{i<j} |z_i − z_j|        — HS-Recurrence field
//                   (O(N²) per pixel; kernel caps N for this metric)
//
// The returned `IterResult` is the common shape for all metrics. Kernels and
// colormap code read whichever fields their metric populated.

#pragma once

#include "complex.hpp"
#include "fx64_raw.hpp"
#include "variants.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <type_traits>
#include <vector>

namespace fsd::compute {

enum class Metric {
    Escape          = 0,
    MinAbs          = 1,
    MaxAbs          = 2,
    Envelope        = 3,
    MinPairwiseDist = 4,
};

inline const char* metric_name(Metric m) {
    switch (m) {
        case Metric::Escape:          return "escape";
        case Metric::MinAbs:          return "min_abs";
        case Metric::MaxAbs:          return "max_abs";
        case Metric::Envelope:        return "envelope";
        case Metric::MinPairwiseDist: return "min_pairwise_dist";
    }
    return "escape";
}

inline bool metric_from_name(const char* name, Metric& out) {
    struct Entry { const char* n; Metric m; };
    static constexpr Entry table[] = {
        {"escape",            Metric::Escape},
        {"min_abs",           Metric::MinAbs},
        {"max_abs",           Metric::MaxAbs},
        {"envelope",          Metric::Envelope},
        {"min_pairwise_dist", Metric::MinPairwiseDist},
    };
    for (const auto& e : table) {
        const char* a = e.n;
        const char* b = name;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (*a == 0 && *b == 0) { out = e.m; return true; }
    }
    return false;
}

using IterResultMask = uint32_t;

namespace IterResultField {
    inline constexpr IterResultMask Iter     = 1u << 0;
    inline constexpr IterResultMask MinAbs   = 1u << 1;
    inline constexpr IterResultMask MaxAbs   = 1u << 2;
    inline constexpr IterResultMask Extra    = 1u << 3;
    inline constexpr IterResultMask Norm     = 1u << 4;
    inline constexpr IterResultMask Escaped  = 1u << 5;
}

constexpr bool iter_result_wants(IterResultMask mask, IterResultMask field) {
    return (mask & field) != 0;
}

// Per-pixel result. `valid_mask` reports which fields were requested and
// maintained by the caller-selected mask.
struct IterResult {
    int    iter;     // escape iteration (== max_iter if bounded)
    double min_abs;  // min |z_n|, valid when valid_mask has MinAbs
    double max_abs;  // max |z_n|, valid for MaxAbs/Envelope
    double extra;    // MinPairwiseDist result
    double norm;     // |z|² at the escape step (0 if not escaped); used by LnSmooth
    bool   escaped;
    IterResultMask valid_mask;
};

template <IterResultMask NeedMask>
inline IterResult make_iter_result() {
    IterResult r{};
    r.iter    = 0;
    r.min_abs = std::numeric_limits<double>::infinity();
    r.max_abs = 0.0;
    r.extra   = std::numeric_limits<double>::infinity();
    r.norm    = 0.0;
    r.escaped = false;
    r.valid_mask = 0;

    if constexpr (iter_result_wants(NeedMask, IterResultField::Iter))    r.valid_mask |= IterResultField::Iter;
    if constexpr (iter_result_wants(NeedMask, IterResultField::MinAbs))  r.valid_mask |= IterResultField::MinAbs;
    if constexpr (iter_result_wants(NeedMask, IterResultField::MaxAbs))  r.valid_mask |= IterResultField::MaxAbs;
    if constexpr (iter_result_wants(NeedMask, IterResultField::Extra))   r.valid_mask |= IterResultField::Extra;
    if constexpr (iter_result_wants(NeedMask, IterResultField::Norm))    r.valid_mask |= IterResultField::Norm;
    if constexpr (iter_result_wants(NeedMask, IterResultField::Escaped)) r.valid_mask |= IterResultField::Escaped;
    return r;
}

inline uint64_t add_u64_sat(uint64_t a, uint64_t b) noexcept {
    const uint64_t sum = a + b;
    return sum < a ? std::numeric_limits<uint64_t>::max() : sum;
}

inline int64_t u64_to_i64_sat(uint64_t value) noexcept {
    if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return std::numeric_limits<int64_t>::max();
    }
    return static_cast<int64_t>(value);
}

template <IterResultMask NeedMask, int FRAC>
inline void finish_fixed_escape(
    IterResult& r,
    int iter,
    uint64_t mag2_raw,
    uint64_t min_mag2_raw,
    uint64_t max_mag2_raw
) {
    if constexpr (iter_result_wants(NeedMask, IterResultField::Iter))    r.iter = iter;
    if constexpr (iter_result_wants(NeedMask, IterResultField::Norm))    r.norm = fixed_mag2_q_to_double<FRAC>(mag2_raw);
    if constexpr (iter_result_wants(NeedMask, IterResultField::Escaped)) r.escaped = true;
    if constexpr (iter_result_wants(NeedMask, IterResultField::MinAbs))  r.min_abs = fixed_mag2_q_to_abs<FRAC>(min_mag2_raw);
    if constexpr (iter_result_wants(NeedMask, IterResultField::MaxAbs))  r.max_abs = fixed_mag2_q_to_abs<FRAC>(max_mag2_raw);
}

enum class FixedEscapeGate {
    Direct,
    Component,
    L1,
};

template <int FRAC, FixedEscapeGate Gate, IterResultMask NeedMask, Variant V>
inline IterResult iterate_fixed_int_masked(
    Cx<Fixed64<FRAC>> z,
    const Cx<Fixed64<FRAC>>& c,
    int max_iter,
    uint64_t bailout_raw,
    uint64_t bailout2_raw,
    uint64_t two_raw = 0,
    uint64_t two_sqrt2_floor_raw = 0,
    bool precheck_initial_z0 = false
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
            return r;
        }
    }

    if (x2_raw == 0 && y2_raw == 0 && (x.raw != 0 || y.raw != 0)) {
        x2_raw = fixed_square_q_sat_raw_cpu<FRAC>(x.raw);
        y2_raw = fixed_square_q_sat_raw_cpu<FRAC>(y.raw);
    }
    S x2{u64_to_i64_sat(x2_raw)};
    S y2{u64_to_i64_sat(y2_raw)};

    for (int i = 0; i < max_iter; i++) {
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
            return r;
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
    return r;
}

template <IterResultMask NeedMask, Variant V>
inline IterResult iterate_fx64_int_masked(
    Cx<Fx64> z,
    const Cx<Fx64>& c,
    int max_iter,
    uint64_t bailout_raw,
    uint64_t bailout2_q57
) {
    return iterate_fixed_int_masked<57, FixedEscapeGate::Direct, NeedMask, V>(
        z, c, max_iter, bailout_raw, bailout2_q57);
}

template <IterResultMask NeedMask, Variant V, typename S>
inline IterResult iterate_quadratic_cached_masked(
    Cx<S> z,
    const Cx<S>& c,
    int max_iter,
    double bailout_sq
) {
    static_assert(!iter_result_wants(NeedMask, IterResultField::Extra),
        "Use iterate_pairwise for MinPairwiseDist.");
    static_assert(!is_fixed64_v<S>,
        "Use iterate_fixed_int_masked for fixed-point render paths.");
    static_assert(!variant_is_transcendental_v<V>(),
        "iterate_quadratic_cached_masked only supports quadratic variants.");

    IterResult r = make_iter_result<NeedMask>();

    S x = z.re;
    S y = z.im;
    S x2 = x * x;
    S y2 = y * y;
    const S bailout_sq_s = scalar_from_double<S>(bailout_sq);

    for (int i = 0; i < max_iter; i++) {
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
            return r;
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
    return r;
}

// Iterate up to max_iter steps of variant V on seed (z0, c). NeedMask is a
// compile-time contract: unused metric maintenance is compiled out of hot loops.
template <IterResultMask NeedMask, Variant V, typename S>
inline IterResult iterate_masked(
    Cx<S> z,
    const Cx<S>& c,
    int max_iter,
    double bailout,
    double bailout_sq
) {
    static_assert(!iter_result_wants(NeedMask, IterResultField::Extra),
        "Use iterate_pairwise for MinPairwiseDist.");
    static_assert(!is_fixed64_v<S>,
        "Use iterate_fixed_int_masked for fixed-point render paths.");

    if constexpr (!variant_is_transcendental_v<V>()) {
        return iterate_quadratic_cached_masked<NeedMask, V, S>(
            z, c, max_iter, bailout_sq);
    } else {

    IterResult r = make_iter_result<NeedMask>();

    for (int i = 0; i < max_iter; i++) {
        z = variant_step<V, S>(z, c);
        const double zre = scalar_to_double(z.re);
        const double zim = scalar_to_double(z.im);
        const bool finite_z = std::isfinite(zre) && std::isfinite(zim);
        const double n2  = finite_z
            ? (zre * zre + zim * zim)
            : std::numeric_limits<double>::infinity();

        if constexpr (iter_result_wants(NeedMask, IterResultField::MinAbs)) {
            if (n2 < r.min_abs) r.min_abs = n2;
        }
        if constexpr (iter_result_wants(NeedMask, IterResultField::MaxAbs)) {
            if (n2 > r.max_abs) r.max_abs = n2;
        }

        bool escaped_now = !finite_z;
        if constexpr (variant_is_transcendental_v<V>()) {
            const double component = std::max(std::fabs(zre), std::fabs(zim));
            escaped_now = escaped_now || component >= bailout;
        } else {
            escaped_now = escaped_now || n2 > bailout_sq;
        }

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
            return r;
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
    return r;
    }
}

template <Variant V, typename S>
inline IterResult iterate_pairwise(
    Cx<S> z,
    const Cx<S>& c,
    int max_iter,
    double bailout,
    double bailout_sq,
    int pairwise_cap,
    std::vector<Cx<S>>& orbit_scratch
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

    if (pairwise_cap <= 0) return r;
    orbit_scratch.clear();
    orbit_scratch.reserve(static_cast<size_t>(pairwise_cap));

    const int n_iter = std::min(max_iter, pairwise_cap);
    for (int i = 0; i < n_iter; i++) {
        z = variant_step<V, S>(z, c);
        const double zre = scalar_to_double(z.re);
        const double zim = scalar_to_double(z.im);
        const bool finite_z = std::isfinite(zre) && std::isfinite(zim);
        const double n2  = finite_z
            ? (zre * zre + zim * zim)
            : std::numeric_limits<double>::infinity();

        for (const auto& prior : orbit_scratch) {
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
    return r;
}

} // namespace fsd::compute
