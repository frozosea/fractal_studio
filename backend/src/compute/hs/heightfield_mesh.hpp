// compute/hs/heightfield_mesh.hpp
//
// Heightfield → indexed triangle mesh. Given a 2D scalar field of size (w × h)
// and world-space extents, produce a uniform grid mesh with each grid cell
// split into two triangles. The scalar value at grid (i, j) becomes the z
// coordinate of vertex (i, j).
//
// This is the shared meshing path for all HS metrics that emit a heightfield
// (min_abs = HS-Base, max_abs, envelope, min_pairwise_dist = HS-Recurrence).

#pragma once

#include "../mesh.hpp"
#include "../escape_time.hpp"
#include "../variants.hpp"

#include <string>

namespace fsd::compute::hs {

struct HsMeshParams {
    double center_re = -0.75;
    double center_im =  0.0;
    double scale     =  3.0;          // complex units, the height of the field
    int    resolution = 192;          // grid size (resolution × resolution)
    int    iterations = 512;
    double bailout    = 2.0;          // radius, kept for metric normalization
    double bailout_sq = 4.0;          // squared threshold used by escape tests

    Variant variant   = Variant::Mandelbrot;
    Metric  metric    = Metric::MinAbs;

    // Orbit length cap for MinPairwiseDist. The metric is O(cap^2) per pixel,
    // so the default stays interactive while callers can opt into deeper runs.
    int pairwise_cap = 64;

    // Z-axis scaling applied to the field values. Larger = taller geometry.
    double heightScale = 0.6;
    // Clamp to prevent spikes from the escape-time metric (which can go to
    // the bailout radius). Field values above this collapse to this value.
    double heightClamp = 2.0;

    // CPU render threads. 0 means auto-select from visible logical cores.
    int render_threads = 0;
};

// Compute the raw metric field values (float64[resolution²], row-major).
// Values are clamped to [0, heightClamp] but not normalized.
// Exposed so the HTTP route can return raw field data without meshing.
void computeHsField(const HsMeshParams& p, std::vector<double>& field);

// Compute the HS heightfield and mesh it. Returns a centered mesh in X,Y
// roughly in [-0.5, +0.5] and Z scaled by heightScale, ready to be written to
// GLB/STL.
Mesh buildHsMesh(const HsMeshParams& p);

} // namespace fsd::compute::hs
