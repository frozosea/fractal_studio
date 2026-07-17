// compute/fx64_raw.hpp
//
// Host-side raw helpers for signed 64-bit fixed-point render paths.  These
// helpers keep viewport generation, escape checks, and field metrics in integer
// space; doubles are only used at API boundaries and final output conversion.

#pragma once

#include "scalar/fx64.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

namespace fsd::compute {

inline constexpr uint64_t TWO_Q60 = 0x2000000000000000ULL;
inline constexpr uint64_t TWO_SQRT2_Q60_FLOOR = 0x2D413CCCFE779921ULL;

template <int FRAC>
struct FixedViewportRaw {
    int64_t center_re_raw = 0;
    int64_t center_im_raw = 0;
    int64_t span_re_raw = 0;
    int64_t span_im_raw = 0;
    int32_t width = 0;
    int32_t height = 0;
    int64_t first_re_raw = 0;
    int64_t first_im_raw = 0;
    int64_t step_re_raw = 0;
    int64_t step_im_raw = 0;
    int64_t julia_re_raw = 0;
    int64_t julia_im_raw = 0;
    uint64_t bailout_raw = 0;
    uint64_t bailout2_raw = 0;
    uint64_t two_raw = 0;
    uint64_t two_sqrt2_floor_raw = 0;

    // Compatibility name for existing Q6.57 callers.
    uint64_t bailout2_q57 = 0;
};

using Fx64ViewportRaw = FixedViewportRaw<57>;

inline uint64_t abs_i64_to_u64(int64_t x) noexcept {
    const uint64_t ux = static_cast<uint64_t>(x);
    return x < 0 ? (~ux + 1ULL) : ux;
}

template <int FRAC>
inline uint64_t fixed_square_q_sat_raw_cpu(int64_t raw) noexcept {
    static_assert(FRAC > 0 && FRAC < 63, "Fixed64 requires 0 < FRAC < 63.");
    const uint64_t a = abs_i64_to_u64(raw);
    const unsigned __int128 product =
        static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(a);
    const unsigned __int128 shifted = product >> FRAC;
    if (shifted > static_cast<unsigned __int128>(std::numeric_limits<uint64_t>::max())) {
        return std::numeric_limits<uint64_t>::max();
    }
    return static_cast<uint64_t>(shifted);
}

template <int FRAC>
inline uint64_t fixed_square_q_sat_raw(int64_t raw) noexcept {
    return fixed_square_q_sat_raw_cpu<FRAC>(raw);
}

template <int FRAC>
inline uint64_t fixed_mag2_q_sat_cpu(int64_t re_raw, int64_t im_raw) noexcept {
    const uint64_t re2 = fixed_square_q_sat_raw_cpu<FRAC>(re_raw);
    const uint64_t im2 = fixed_square_q_sat_raw_cpu<FRAC>(im_raw);
    const uint64_t sum = re2 + im2;
    if (sum < re2) return std::numeric_limits<uint64_t>::max();
    return sum;
}

template <int FRAC>
inline uint64_t fixed_mag2_q_sat(int64_t re_raw, int64_t im_raw) noexcept {
    return fixed_mag2_q_sat_cpu<FRAC>(re_raw, im_raw);
}

template <int FRAC>
inline bool fixed_component_escaped_q(
    int64_t re_raw,
    int64_t im_raw,
    uint64_t bailout_raw
) noexcept {
    (void)FRAC;
    return abs_i64_to_u64(re_raw) > bailout_raw ||
           abs_i64_to_u64(im_raw) > bailout_raw;
}

template <int FRAC>
inline bool fixed_escaped_q_cpu(
    int64_t re_raw,
    int64_t im_raw,
    uint64_t bailout2_raw
) noexcept {
    return fixed_mag2_q_sat_cpu<FRAC>(re_raw, im_raw) > bailout2_raw;
}

template <int FRAC>
inline bool fixed_escaped_q(
    int64_t re_raw,
    int64_t im_raw,
    uint64_t bailout2_raw
) noexcept {
    return fixed_escaped_q_cpu<FRAC>(re_raw, im_raw, bailout2_raw);
}

template <int FRAC>
inline double fixed_mag2_q_to_double(uint64_t mag2_raw) noexcept {
    return static_cast<double>(mag2_raw) / Fixed64<FRAC>::SCALE;
}

template <int FRAC>
inline double fixed_mag2_q_to_abs(uint64_t mag2_raw) noexcept {
    return std::sqrt(fixed_mag2_q_to_double<FRAC>(mag2_raw));
}

template <int FRAC>
inline int64_t fixed_round_to_raw_sat(double x) noexcept {
    if (!std::isfinite(x)) {
        return x < 0.0 ? std::numeric_limits<int64_t>::min()
                       : std::numeric_limits<int64_t>::max();
    }
    const long double scaled = static_cast<long double>(x) * std::ldexp(1.0L, FRAC);
    if (scaled >= static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        return std::numeric_limits<int64_t>::max();
    }
    if (scaled <= static_cast<long double>(std::numeric_limits<int64_t>::min())) {
        return std::numeric_limits<int64_t>::min();
    }
    return static_cast<int64_t>(std::llround(scaled));
}

template <int FRAC>
inline uint64_t fixed_round_to_uraw_sat(double x) noexcept {
    if (!std::isfinite(x) || x <= 0.0) return 0;
    const long double scaled = static_cast<long double>(x) * std::ldexp(1.0L, FRAC);
    if (scaled >= static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
        return std::numeric_limits<uint64_t>::max();
    }
    return static_cast<uint64_t>(std::floor(scaled + 0.5L));
}

inline int64_t fixed_saturate_i128(__int128 value) noexcept {
    if (value > static_cast<__int128>(std::numeric_limits<int64_t>::max())) {
        return std::numeric_limits<int64_t>::max();
    }
    if (value < static_cast<__int128>(std::numeric_limits<int64_t>::min())) {
        return std::numeric_limits<int64_t>::min();
    }
    return static_cast<int64_t>(value);
}

template <int FRAC>
inline constexpr uint64_t fixed_two_raw_const() noexcept {
    static_assert(FRAC < 63, "Fixed64 requires FRAC < 63.");
    return 1ULL << (FRAC + 1);
}

template <int FRAC>
inline constexpr uint64_t fixed_two_sqrt2_floor_raw_const() noexcept {
    if constexpr (FRAC == 60) {
        return TWO_SQRT2_Q60_FLOOR;
    } else {
        return 0;
    }
}

template <int FRAC>
inline FixedViewportRaw<FRAC> make_fixed_viewport_raw(
    double center_re,
    double center_im,
    double scale,
    double viewport_aspect,
    int width,
    int height,
    double julia_re,
    double julia_im,
    double bailout,
    double bailout_sq
) noexcept {
    FixedViewportRaw<FRAC> v;
    const int64_t center_re_raw = fixed_round_to_raw_sat<FRAC>(center_re);
    const int64_t center_im_raw = fixed_round_to_raw_sat<FRAC>(center_im);
    const int64_t scale_raw = fixed_round_to_raw_sat<FRAC>(scale);
    const int64_t span_re_value_raw =
        fixed_round_to_raw_sat<FRAC>(scale * viewport_aspect);

    const __int128 span_im_raw = static_cast<__int128>(scale_raw);
    const __int128 span_re_raw = static_cast<__int128>(span_re_value_raw);
    v.center_re_raw = center_re_raw;
    v.center_im_raw = center_im_raw;
    v.span_re_raw = span_re_value_raw;
    v.span_im_raw = scale_raw;
    v.width = width;
    v.height = height;
    v.step_re_raw = width > 0
        ? fixed_saturate_i128(span_re_raw / width)
        : 0;
    v.step_im_raw = height > 0
        ? fixed_saturate_i128(span_im_raw / height)
        : 0;

    v.first_re_raw = fixed_saturate_i128(
        static_cast<__int128>(center_re_raw) - span_re_raw / 2 + v.step_re_raw / 2);
    v.first_im_raw = fixed_saturate_i128(
        static_cast<__int128>(center_im_raw) + span_im_raw / 2 - v.step_im_raw / 2);
    v.julia_re_raw = fixed_round_to_raw_sat<FRAC>(julia_re);
    v.julia_im_raw = fixed_round_to_raw_sat<FRAC>(julia_im);
    v.bailout_raw = fixed_round_to_uraw_sat<FRAC>(bailout);
    v.bailout2_raw = fixed_round_to_uraw_sat<FRAC>(bailout_sq);
    v.two_raw = fixed_two_raw_const<FRAC>();
    v.two_sqrt2_floor_raw = fixed_two_sqrt2_floor_raw_const<FRAC>();
    v.bailout2_q57 = v.bailout2_raw;
    return v;
}

template <int FRAC>
inline int64_t fixed_pixel_re_raw(const FixedViewportRaw<FRAC>& v, int x) noexcept {
    if (v.width > 0) {
        const int64_t denominator = 2LL * v.width;
        const int64_t numerator_factor = 2LL * x + 1LL;
        const int64_t quotient = v.span_re_raw / denominator;
        const int64_t remainder = v.span_re_raw % denominator;
        const int64_t offset = quotient * numerator_factor +
            (remainder * numerator_factor) / denominator;
        return v.center_re_raw - v.span_re_raw / 2 + offset;
    }
    return fixed_saturate_i128(
        static_cast<__int128>(v.first_re_raw) +
        static_cast<__int128>(x) * v.step_re_raw);
}

template <int FRAC>
inline int64_t fixed_pixel_im_raw(const FixedViewportRaw<FRAC>& v, int y) noexcept {
    if (v.height > 0) {
        const int64_t denominator = 2LL * v.height;
        const int64_t numerator_factor = 2LL * y + 1LL;
        const int64_t quotient = v.span_im_raw / denominator;
        const int64_t remainder = v.span_im_raw % denominator;
        const int64_t offset = quotient * numerator_factor +
            (remainder * numerator_factor) / denominator;
        return v.center_im_raw + v.span_im_raw / 2 - offset;
    }
    return fixed_saturate_i128(
        static_cast<__int128>(v.first_im_raw) -
        static_cast<__int128>(y) * v.step_im_raw);
}

inline uint64_t fx64_square_q57_sat_raw_cpu(int64_t raw) noexcept {
    return fixed_square_q_sat_raw_cpu<57>(raw);
}

inline uint64_t fx64_square_q57_sat_raw(int64_t raw) noexcept {
    return fixed_square_q_sat_raw<57>(raw);
}

inline uint64_t fx64_mag2_q57_sat_cpu(int64_t re_raw, int64_t im_raw) noexcept {
    return fixed_mag2_q_sat_cpu<57>(re_raw, im_raw);
}

inline uint64_t fx64_mag2_q57_sat(int64_t re_raw, int64_t im_raw) noexcept {
    return fixed_mag2_q_sat<57>(re_raw, im_raw);
}

inline bool fx64_component_escaped_q57(
    int64_t re_raw,
    int64_t im_raw,
    uint64_t bailout_raw
) noexcept {
    return fixed_component_escaped_q<57>(re_raw, im_raw, bailout_raw);
}

inline bool fx64_escaped_q57_cpu(
    int64_t re_raw,
    int64_t im_raw,
    uint64_t bailout2_q57
) noexcept {
    return fixed_escaped_q_cpu<57>(re_raw, im_raw, bailout2_q57);
}

inline bool fx64_escaped_q57(
    int64_t re_raw,
    int64_t im_raw,
    uint64_t bailout2_q57
) noexcept {
    return fx64_escaped_q57_cpu(re_raw, im_raw, bailout2_q57);
}

inline double fx64_mag2_q57_to_double(uint64_t mag2_q57) noexcept {
    return fixed_mag2_q_to_double<57>(mag2_q57);
}

inline double fx64_mag2_q57_to_abs(uint64_t mag2_q57) noexcept {
    return fixed_mag2_q_to_abs<57>(mag2_q57);
}

inline int64_t fx64_round_to_raw_sat(double x) noexcept {
    return fixed_round_to_raw_sat<57>(x);
}

inline uint64_t fx64_round_to_uraw_sat(double x) noexcept {
    return fixed_round_to_uraw_sat<57>(x);
}

inline int64_t fx64_saturate_i128(__int128 value) noexcept {
    return fixed_saturate_i128(value);
}

inline Fx64ViewportRaw make_fx64_viewport_raw(
    double center_re,
    double center_im,
    double scale,
    double viewport_aspect,
    int width,
    int height,
    double julia_re,
    double julia_im,
    double bailout,
    double bailout_sq
) noexcept {
    return make_fixed_viewport_raw<57>(
        center_re, center_im, scale, viewport_aspect, width, height,
        julia_re, julia_im, bailout, bailout_sq);
}

inline int64_t fx64_pixel_re_raw(const Fx64ViewportRaw& v, int x) noexcept {
    return fixed_pixel_re_raw<57>(v, x);
}

inline int64_t fx64_pixel_im_raw(const Fx64ViewportRaw& v, int y) noexcept {
    return fixed_pixel_im_raw<57>(v, y);
}

} // namespace fsd::compute
