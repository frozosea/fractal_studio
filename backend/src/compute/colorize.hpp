// compute/colorize.hpp
//
// Unified colorization layer. All compute engines output raw numeric data
// (FieldOutput); this module converts those numbers to BGR images.
//
// Decouples iteration kernels from color mapping, so new coloring modes
// (equalized, bands, etc.) don't require changes to every engine.

#pragma once

#include "map_kernel.hpp"

#include <opencv2/core.hpp>

#include <string>

namespace fsd::compute {

struct LnMapEqualization;

// Unified entry point. Dispatches to colorize_direct or colorize_equalized
// based on color_mode.
//   "direct"    → classic per-pixel escape/field coloring
//   "eq_full"   → histogram-equalized periodic coloring (full image)
//   "eq_center" → histogram-equalized periodic coloring (center weighted)
cv::Mat colorize_field(const MapParams& p, const FieldOutput& field,
                       const std::string& color_mode = "direct",
                       double cycles_per_octave = 1.0);

// Classic coloring: escape metric uses colorize_escape_bgr (with optional
// smooth), non-escape metrics use colorize_field_bgr / _hs / _smooth.
cv::Mat colorize_direct(const MapParams& p, const FieldOutput& field);

// Histogram-equalized periodic coloring. Builds an LnMapEqualization from
// the escape-count field, then maps each pixel through it.
cv::Mat colorize_equalized(const MapParams& p, const FieldOutput& field,
                           bool distance_weighted, double cycles_per_octave);

} // namespace fsd::compute
