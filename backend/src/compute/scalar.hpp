// compute/scalar.hpp
//
// Scalar type abstraction for fractal kernels.
//
// Kernels use these overload points with floating-point scalars and the Fx64
// fixed-point family. Keeping conversions and elementary operations here lets
// templated kernels preserve the selected scalar instead of passing through
// double accidentally.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#if defined(FSD_HAS_FLOAT128)
#  include <quadmath.h>
#endif

namespace fsd::compute {

// Default scalar for callers that do not select a concrete representation.
using Scalar = double;

// Scalar helpers. Fixed64 adds matching overloads from scalar/fx64.hpp below.
inline double scalar_abs(double x) noexcept { return std::fabs(x); }
inline double scalar_sqrt(double x) noexcept { return std::sqrt(x); }
inline double scalar_sin(double x) noexcept { return std::sin(x); }
inline double scalar_cos(double x) noexcept { return std::cos(x); }
inline double scalar_from_double(double x) noexcept { return x; }
inline double scalar_to_double(double x) noexcept { return x; }
inline float scalar_abs(float x) noexcept { return std::fabs(x); }
inline float scalar_sqrt(float x) noexcept { return std::sqrt(x); }
inline float scalar_sin(float x) noexcept { return std::sin(x); }
inline float scalar_cos(float x) noexcept { return std::cos(x); }
inline double scalar_to_double(float x) noexcept { return static_cast<double>(x); }
inline long double scalar_abs(long double x) noexcept { return std::fabs(x); }
inline long double scalar_sqrt(long double x) noexcept { return std::sqrt(x); }
inline long double scalar_sin(long double x) noexcept { return std::sin(x); }
inline long double scalar_cos(long double x) noexcept { return std::cos(x); }
inline double scalar_to_double(long double x) noexcept { return static_cast<double>(x); }

#if defined(FSD_HAS_FLOAT128)
inline __float128 scalar_abs(__float128 x) noexcept { return fabsq(x); }
inline __float128 scalar_sqrt(__float128 x) noexcept { return sqrtq(x); }
inline __float128 scalar_sin(__float128 x) noexcept { return sinq(x); }
inline __float128 scalar_cos(__float128 x) noexcept { return cosq(x); }
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

// Bring in fixed-point scalars so callers get every supported representation.
#include "scalar/fx64.hpp"
