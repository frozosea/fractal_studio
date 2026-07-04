// compute/perturbation.hpp
//
// Perturbation-theory renderer for deep-zoom Mandelbrot and Julia.
//
// Computes reference orbits at high precision (double / __float128 / MPFR
// tiered by scale), then uses fp64 perturbation deltas (delta_z) for all
// pixels. This enables rendering at zoom depths far beyond fp64's ~1e-15
// limit while keeping per-pixel work in fast fp64 arithmetic.
//
// Pixels use Zhuoran-style rebasing: whenever the full orbit |Z_m + dz|
// drops below |dz|, or the reference orbit data runs out, the pixel is
// re-expressed as a perturbation of the start of the *critical* orbit
// (exact, since its Z_0 = 0 is the critical point). One pass, no glitch
// heuristics, no correction passes:
//
//   Mandelbrot: the reference orbit at the viewport center starts at the
//     critical point already, so it serves as both the primary and the
//     rebase target (R == K below).
//   Julia: pixels start against the seeded orbit R of the viewport center
//     (Z_0 = center, tiny initial dz), and on the first rebase switch to the
//     critical orbit K of c_julia (Z_0 = 0), rebasing within K thereafter.
//
// Only the standard Mandelbrot map (z^2 + c) is supported. Other quadratic
// variants break analyticity with abs-value folds, making the perturbation
// formula piecewise and significantly more complex.

#pragma once

#include "map_kernel.hpp"

#include <opencv2/core.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
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

// Precision-tiered overload: selects double / __float128 / MPFR based on scale.
// String coordinates parsed at full precision for the reference orbit.
RefOrbit compute_reference_orbit_auto(
    const std::string& center_re_str, const std::string& center_im_str,
    int max_iter, double bailout_sq, double scale);

RefOrbit compute_reference_orbit_julia(
    double z0_re, double z0_im,
    double julia_re, double julia_im,
    int max_iter, double bailout_sq);

RefOrbit compute_reference_orbit_julia_auto(
    const std::string& z0_re_str, const std::string& z0_im_str,
    double julia_re, double julia_im,
    int max_iter, double bailout_sq, double scale);

// ---------------------------------------------------------------------------
// Per-pixel fp64 delta iteration with rebasing, shared by the cartesian and
// ln-map renderers (and mirrored by the AVX2/CUDA kernels).
// ---------------------------------------------------------------------------

struct PerturbPixel {
    int     iter        = 0;      // escape iteration; == max_iter if inside
    bool    escaped     = false;
    double  escape_mag2 = 0.0;    // |z|^2 at escape (smooth/equalized coloring)
    // Populated only by perturb_iterate<true> (min/max metrics): extrema of
    // |z_n|^2 over the post-step orbit values z_1..z_N, matching
    // iterate_quadratic_cached_masked (z_0 excluded, escape step included).
    double  min_mag2    = std::numeric_limits<double>::infinity();
    double  max_mag2    = 0.0;
};

// Iterate one pixel.
//   R          primary reference orbit (Mandelbrot: center orbit; Julia:
//              seeded orbit of the viewport center).
//   K          rebase orbit whose Z_0 = 0 (Mandelbrot: same as R; Julia:
//              critical orbit of c_julia). Pass R again for Mandelbrot.
//   dz0        initial delta (Mandelbrot: 0; Julia: pixel offset from center).
//   dc         pixel's c offset from the reference c (Mandelbrot: pixel
//              offset; Julia: 0 — every pixel shares c_julia exactly).
//
// TrackMinMax=true additionally records min/max |z|^2 over the orbit (the
// MinAbs / MaxAbs / Envelope metrics — exact under perturbation because the
// full orbit value z = Z_m + dz is reconstructed every step anyway).
//
// Glitch-free by construction: rebases onto K's start whenever significance
// would be lost or the current orbit's data ends.
template <bool TrackMinMax = false>
inline PerturbPixel perturb_iterate(
    const double* Rr, const double* Ri, int r_len,
    const double* Kr, const double* Ki, int k_len,
    double dz0_re, double dz0_im,
    double dc_re, double dc_im,
    int max_iter, double bail2) noexcept
{
    PerturbPixel out;
    out.iter = max_iter;

    const double* Or = Rr;
    const double* Oi = Ri;
    int olen = r_len;
    int m = 0;  // reference orbit index, decoupled from pixel iteration n

    double dz_re = dz0_re, dz_im = dz0_im;

    if (olen < 2) {
        // Degenerate primary orbit (its seed already escaped): rebase to K
        // immediately. dz <- z_0 = R_0 + dz is exact since K_0 = 0.
        dz_re = Rr[0] + dz_re;
        dz_im = Ri[0] + dz_im;
        Or = Kr; Oi = Ki; olen = k_len;
        if (olen < 2) return out;
    }

    for (int n = 0; n < max_iter; ++n) {
        const double two_Zr = 2.0 * Or[m];
        const double two_Zi = 2.0 * Oi[m];

        // dz' = 2*Z_m*dz + dz^2 + dc
        const double new_dz_re = two_Zr * dz_re - two_Zi * dz_im
                               + dz_re * dz_re - dz_im * dz_im + dc_re;
        const double new_dz_im = two_Zr * dz_im + two_Zi * dz_re
                               + 2.0 * dz_re * dz_im + dc_im;
        dz_re = new_dz_re;
        dz_im = new_dz_im;

        const double z_re = Or[m + 1] + dz_re;
        const double z_im = Oi[m + 1] + dz_im;
        const double mag2 = z_re * z_re + z_im * z_im;

        if constexpr (TrackMinMax) {
            if (mag2 < out.min_mag2) out.min_mag2 = mag2;
            if (mag2 > out.max_mag2) out.max_mag2 = mag2;
        }

        if (mag2 > bail2) {
            out.iter = n;
            out.escaped = true;
            out.escape_mag2 = mag2;
            return out;
        }

        ++m;
        const double dz_mag2 = dz_re * dz_re + dz_im * dz_im;
        if (mag2 < dz_mag2 || m + 1 >= olen) {
            // Rebase: the full orbit is closer to K_0 = 0 than to Z_m, or the
            // orbit data ends. dz <- z is exact because K_0 = 0.
            dz_re = z_re;
            dz_im = z_im;
            Or = Kr; Oi = Ki; olen = k_len;
            m = 0;
        }
    }
    return out;
}

MapStats render_map_perturbation(const MapParams& p, cv::Mat& out);

MapStats render_map_field_perturbation(const MapParams& p, FieldOutput& fo);

bool perturbation_applicable(const MapParams& p);

} // namespace fsd::compute
