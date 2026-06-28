// compute/transition_kernel.hpp
//
// User's innovation: the continuous 3D bridge between Mandelbrot and Burning
// Ship dynamics (README.md:39).
//
//   x_{n+1} = x² − y² − z² + x₀
//   y_{n+1} = 2·x·y     + y₀
//   z_{n+1} = 2·|x·z|   + z₀
//
// In the xy-plane (z=0) the iteration is exactly Mandelbrot. In the xz-plane
// (y=0) the iteration is Burning-Ship-like (the |·| on the xz cross-term is
// what folds the imaginary axis and produces the ship hull).
//
// A 2D map at rotation angle θ around the x-axis embeds the screen's
// imaginary axis into 3D as (cosθ·y + sinθ·z). Concretely a pixel (u, v)
// maps to seed (x₀, y₀, z₀) = (u, v·cosθ, v·sinθ). θ=0 → the xy-plane
// variant, θ=π/2 → the xz-plane variant, θ=-π/2 and θ=±π are vertically
// flipped degenerate slices. render_transition snaps fixed-point cardinal
// angles to the direct 2D map renderer to avoid tiny non-zero sin/cos drift.
//
// This matches cfiles/mandelbrot_3Dtranslation_minmax.c exactly (see its
// `mandelbrot(c, max, num, theta)` at line 52).

#pragma once

#include "colormap.hpp"
#include "escape_time.hpp"
#include "map_kernel.hpp"  // for MapStats
#include "variants.hpp"

#include <opencv2/core.hpp>

#include <functional>
#include <string>

namespace fsd::compute {

struct TransitionParams {
    MapParams base;

    double theta = 0.0;
    bool theta_milli_deg_set = false;
    int theta_milli_deg = 0;

    Variant from_variant = Variant::Mandelbrot;
    Variant to_variant   = Variant::Boat;
};

MapStats render_transition_field(const TransitionParams& p, FieldOutput& fo);
MapStats render_transition(const TransitionParams& p, cv::Mat& out);

} // namespace fsd::compute
