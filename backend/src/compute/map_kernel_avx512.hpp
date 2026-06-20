// compute/map_kernel_avx512.hpp
//
// AVX-512 accelerated map renderer.
//
// Processes 8 pixels simultaneously using AVX-512 double-precision (zmm
// registers, __m512d). Escape check is done in bulk with masked iteration —
// pixels that have already escaped have their lanes masked off, keeping the
// remaining active lanes churning. This is especially effective when the
// iteration counts across a tile are relatively uniform.
//
// Fx64 on AVX-512 currently falls back to the OpenMP integer path. The old
// IFMA52 route was removed because it converted to fp64 inside the escape
// loop; a future IFMA path must keep escape and metrics in raw Q6.57 form.
//
// Feature detection: callers must check avx512_available() before calling.
// If AVX-512 is not available at runtime the map_kernel.cpp OpenMP path is
// used instead.

#pragma once

#include "map_kernel.hpp"

namespace fsd::compute {

// Returns true if the CPU supports the AVX-512 feature set used by the fp64
// kernel at runtime.
bool avx512_available() noexcept;

// Returns true if AVX-512IFMA is available. The current fx64 renderer does not
// use it, but callers can distinguish fp64 AVX-512 from future IFMA support.
bool avx512ifma_available() noexcept;

// AVX-512 fp64 render (8 pixels at a time).
// Pre-condition: avx512_available() == true.
MapStats render_map_avx512_fp64(const MapParams& p, cv::Mat& out);

// AVX-512 fp64 escape-count FIELD (iter_u32 + |z|² norm_f32). Escape metric only;
// feeds equalized coloring. Pre-condition: avx512_available() == true.
MapStats render_map_field_avx512_fp64(const MapParams& p, FieldOutput& out);

// AVX-512 fp32 render (16 pixels at a time).
// Pre-condition: avx512_available() == true.
MapStats render_map_avx512_fp32(const MapParams& p, cv::Mat& out);

// AVX-512 Fx64 render placeholder; returns openmp_fallback.
MapStats render_map_avx512_fx64(const MapParams& p, cv::Mat& out);

} // namespace fsd::compute
