// compute/perturbation.hpp
//
// Perturbation-theory renderer for deep-zoom Mandelbrot.
//
// Computes a single reference orbit at __float128 precision, then uses fp64
// perturbation deltas (delta_z) for all pixels. This enables rendering at
// zoom depths far beyond fp64's ~1e-15 limit while keeping per-pixel work
// in fast fp64 arithmetic.
//
// Only the standard Mandelbrot (z^2 + c) is supported. Other quadratic
// variants break analyticity with abs-value folds, making the perturbation
// formula piecewise and significantly more complex.

#pragma once

#include "map_kernel.hpp"

#include <opencv2/core.hpp>

#include <cstdint>
#include <vector>

namespace fsd::compute {

struct RefOrbit {
    std::vector<double> z_re;
    std::vector<double> z_im;
    int    length   = 0;
    bool   escaped  = false;
    double bail2    = 4.0;
};

RefOrbit compute_reference_orbit(
    double center_re, double center_im,
    int max_iter, double bailout_sq);

MapStats render_map_perturbation(const MapParams& p, cv::Mat& out);

bool perturbation_applicable(const MapParams& p);

} // namespace fsd::compute
