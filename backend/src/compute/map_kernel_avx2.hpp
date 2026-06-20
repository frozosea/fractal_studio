// compute/map_kernel_avx2.hpp
//
// AVX2/FMA map renderer. Processes 4 fp64 or 8 fp32 pixels at a time.

#pragma once

#include "cpu_features.hpp"
#include "map_kernel.hpp"

namespace fsd::compute {

MapStats render_map_avx2_fp64(const MapParams& p, cv::Mat& out);
MapStats render_map_avx2_fp32(const MapParams& p, cv::Mat& out);

// AVX-2 fp64 escape-count FIELD (iter_u32 + |z|² norm_f32). Escape metric only; feeds
// equalized coloring on CPUs without AVX-512. Pre-condition: avx2_available() && fma_available().
MapStats render_map_avx2_field(const MapParams& p, FieldOutput& out);

} // namespace fsd::compute
