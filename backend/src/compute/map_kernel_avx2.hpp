// compute/map_kernel_avx2.hpp
//
// AVX2/FMA map renderer. Processes 4 fp64 or 8 fp32 pixels at a time.

#pragma once

#include "cpu_features.hpp"
#include "map_kernel.hpp"

namespace fsd::compute {

MapStats render_map_avx2_fp64(const MapParams& p, cv::Mat& out);
MapStats render_map_avx2_fp32(const MapParams& p, cv::Mat& out);

} // namespace fsd::compute
