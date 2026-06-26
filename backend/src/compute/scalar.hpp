// compute/scalar.hpp
//
// Scalar type abstraction for fractal kernels.
//
// Phase 1: `Scalar` is a type alias for `double`. All kernels take a template
// parameter `Scalar` but today we only instantiate them with `double`.
// Phase 3 will add `struct Fx64` (1 sign · 6 integer · 57 fractional bits
// fixed-point) and the same templated kernels will be re-instantiated with it.
//
// The goal of this file is to pin down the *operations* every scalar type must
// support so kernels write `a*a + b*b` and it compiles for both `double` and
// `Fx64` in the future.
//
// No concept/constraints yet — the kernels just call the plain arithmetic
// operators. When `Fx64` lands it will overload those operators and provide
// `fabs` / `sqrt` / conversion helpers in the same namespace.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#if defined(FSD_HAS_FLOAT128)
#  include <quadmath.h>
#endif

namespace fsd::compute {

// Default scalar for Phase 1. All kernels take `template<typename Scalar>` and
// are currently instantiated with `double`.
using Scalar = double;

// Scalar helpers. These are overload points — `Fx64` will provide its own
// overloads in the same namespace in Phase 3.
inline double scalar_abs(double x) noexcept { return std::fabs(x); }
inline double scalar_sqrt(double x) noexcept { return std::sqrt(x); }
inline double scalar_from_double(double x) noexcept { return x; }
inline double scalar_to_double(double x) noexcept { return x; }
inline float scalar_abs(float x) noexcept { return std::fabs(x); }
inline float scalar_sqrt(float x) noexcept { return std::sqrt(x); }
inline double scalar_to_double(float x) noexcept { return static_cast<double>(x); }
inline long double scalar_abs(long double x) noexcept { return std::fabs(x); }
inline long double scalar_sqrt(long double x) noexcept { return std::sqrt(x); }
inline double scalar_to_double(long double x) noexcept { return static_cast<double>(x); }

#if defined(FSD_HAS_FLOAT128)
inline __float128 scalar_abs(__float128 x) noexcept { return fabsq(x); }
inline __float128 scalar_sqrt(__float128 x) noexcept { return sqrtq(x); }
inline double scalar_to_double(__float128 x) noexcept { return static_cast<double>(x); }
#endif

// Template helper: scalar_from_double<S>(double) — converts a double to scalar
// type S. Works for both double (identity) and Fx64 (via static method).
// Usage: scalar_from_double<S>(x)  instead of  static_cast<S>(x)
template <typename S>
inline S scalar_from_double(double x) noexcept {
    return S::from_double(x);  // Overridden below for double.
}

// Specialisation for double (identity).
template <>
inline double scalar_from_double<double>(double x) noexcept { return x; }

// Specialisation for float.
template <>
inline float scalar_from_double<float>(double x) noexcept { return static_cast<float>(x); }

// Specialisation for long double.
template <>
inline long double scalar_from_double<long double>(double x) noexcept { return static_cast<long double>(x); }

#if defined(FSD_HAS_FLOAT128)
// Specialisation for GCC/libquadmath binary128.
template <>
inline __float128 scalar_from_double<__float128>(double x) noexcept { return static_cast<__float128>(x); }
#endif

// ---------- scalar_from_string<S>() ----------
// Parse a decimal string directly into the target scalar type, preserving
// all digits that the type can represent (vs. going through double first).
// Falls back to scalar_from_double when string is empty.

template <typename S>
inline S scalar_from_string(const std::string& s, double fallback) noexcept {
    return scalar_from_double<S>(fallback);
}

template <>
inline double scalar_from_string<double>(const std::string& s, double fallback) noexcept {
    if (s.empty()) return fallback;
    return std::strtod(s.c_str(), nullptr);
}

template <>
inline float scalar_from_string<float>(const std::string& s, double fallback) noexcept {
    if (s.empty()) return static_cast<float>(fallback);
    return std::strtof(s.c_str(), nullptr);
}

template <>
inline long double scalar_from_string<long double>(const std::string& s, double fallback) noexcept {
    if (s.empty()) return static_cast<long double>(fallback);
    return std::strtold(s.c_str(), nullptr);
}

#if defined(FSD_HAS_FLOAT128)
template <>
inline __float128 scalar_from_string<__float128>(const std::string& s, double fallback) noexcept {
    if (s.empty()) return static_cast<__float128>(fallback);
    return strtoflt128(s.c_str(), nullptr);
}
#endif

} // namespace fsd::compute

// Phase 3: bring in the fixed-point scalar so callers can include scalar.hpp
// alone and get both double helpers and Fx64.
#include "scalar/fx64.hpp"
