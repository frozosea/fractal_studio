// compute/perturbation.hpp
//
// Perturbation-theory renderer for deep-zoom Mandelbrot and Julia.
//
// Computes reference orbits at high precision (double / __float128 / MPFR,
// tiered by scale or pinned by the scalar_type combo), then iterates
// per-pixel deltas (delta_z) in a selectable precision — fp32, fp64 or
// __float128. fp64 deltas are the default; fp32 deltas trade the deepest
// range for large SIMD/GPU speedups (consumer GPUs run fp32 32–64x faster
// than fp64); __float128 deltas are an oracle/validation mode.
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
//
// Depth ranges by delta precision (reference orbit precision must also
// cover the depth — see kRefFp128MinScale below):
//   fp32   exact-looking down to scale ~1e-30: |dz| quanta stay above the
//          fp32 denormal floor (2^-149) with the ~2^-24 relative delta
//          precision; degrades gracefully below, unusable past ~1e-38.
//   fp64   exact down to scale ~1e-305 (verified against an MPFR oracle at
//          1.8e-301). The remaining wall is representation, not arithmetic:
//          pixel offsets denormalize below ~1e-305, and MapParams::scale
//          itself is a double (min normal 2.2e-308).
//   fp128  validation mode; extends the arithmetic headroom to the scale
//          representation wall (~2.2e-308). Pixel offsets are computed in
//          __float128 for this mode, so dc carries full 113-bit precision.

#pragma once

#include "map_kernel.hpp"

#include <opencv2/core.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace fsd::compute {

// Auto reference-orbit tier: keep __float128 only while it retains a healthy
// guard margin. The reference orbit is iterated at 113-bit precision and its
// error compounds with the orbit's Lyapunov growth (~1/scale near escape), so
// the usable depth is mantissa bits minus a guard band. Rebase-seam tearing
// was observed in the field at ~110 octaves (scale ~3e-33, i.e. ~108 zoom
// bits — only ~5 guard bits) under the old 1e-33 cutoff. 1e-26 (~86 zoom
// bits) keeps >= ~26 guard bits; below it the auto tier switches to MPFR,
// whose precision tracks the scale (zoom bits + 64).
inline constexpr double kRefFp128MinScale = 1e-26;

struct RefOrbit {
    std::vector<double> z_re;
    std::vector<double> z_im;
    int    length   = 0;
    bool   escaped  = false;
    double bail2    = 4.0;
    // Precision actually used to iterate the orbit: "fp64", "fp128", or
    // "mpfr<bits>" (e.g. "mpfr192"). Reported through MapStats::scalar_used.
    std::string prec_used = "fp64";
};

RefOrbit compute_reference_orbit(
    double center_re, double center_im,
    int max_iter, double bailout_sq);

// Scale-aware variant for double-defined centers (Julia critical orbits, API
// calls without decimal strings): tiers fp128 / MPFR like the string path —
// the double center is exact in every tier, but the *iteration* still needs
// enough precision for the requested depth. `ref_prec` pins the tier.
RefOrbit compute_reference_orbit_scaled(
    double center_re, double center_im,
    int max_iter, double bailout_sq, double scale,
    PerturbRefPrec ref_prec = PerturbRefPrec::Auto);

// Precision-tiered overload: selects fp128 / MPFR based on scale (or the
// explicit `ref_prec`). String coordinates parsed at full precision.
RefOrbit compute_reference_orbit_auto(
    const std::string& center_re_str, const std::string& center_im_str,
    int max_iter, double bailout_sq, double scale,
    PerturbRefPrec ref_prec = PerturbRefPrec::Auto);

RefOrbit compute_reference_orbit_julia(
    double z0_re, double z0_im,
    double julia_re, double julia_im,
    int max_iter, double bailout_sq);

RefOrbit compute_reference_orbit_julia_auto(
    const std::string& z0_re_str, const std::string& z0_im_str,
    double julia_re, double julia_im,
    int max_iter, double bailout_sq, double scale,
    PerturbRefPrec ref_prec = PerturbRefPrec::Auto);

// ---------------------------------------------------------------------------
// Per-pixel delta iteration with rebasing, shared by the cartesian and
// ln-map renderers (and mirrored by the AVX2/AVX-512/CUDA kernels). The
// delta scalar D is fp32 / fp64 / __float128; reference tables are stored in
// D as well (the driver converts once per render).
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
template <bool TrackMinMax = false, typename D = double>
inline PerturbPixel perturb_iterate(
    const D* Rr, const D* Ri, int r_len,
    const D* Kr, const D* Ki, int k_len,
    D dz0_re, D dz0_im,
    D dc_re, D dc_im,
    int max_iter, D bail2) noexcept
{
    PerturbPixel out;
    out.iter = max_iter;

    const D* Or = Rr;
    const D* Oi = Ri;
    int olen = r_len;
    int m = 0;  // reference orbit index, decoupled from pixel iteration n

    D dz_re = dz0_re, dz_im = dz0_im;

    // double inf converts to D's inf for every supported D (fp32/fp64/fp128),
    // avoiding a numeric_limits<__float128> dependency in strict-ANSI builds.
    D min_mag2 = D(std::numeric_limits<double>::infinity());
    D max_mag2 = D(0);

    if (olen < 2) {
        // Degenerate primary orbit (its seed already escaped): rebase to K
        // immediately. dz <- z_0 = R_0 + dz is exact since K_0 = 0.
        dz_re = Rr[0] + dz_re;
        dz_im = Ri[0] + dz_im;
        Or = Kr; Oi = Ki; olen = k_len;
        if (olen < 2) return out;
    }

    const D two = D(2);
    for (int n = 0; n < max_iter; ++n) {
        const D two_Zr = two * Or[m];
        const D two_Zi = two * Oi[m];

        // dz' = 2*Z_m*dz + dz^2 + dc
        const D new_dz_re = two_Zr * dz_re - two_Zi * dz_im
                          + dz_re * dz_re - dz_im * dz_im + dc_re;
        const D new_dz_im = two_Zr * dz_im + two_Zi * dz_re
                          + two * dz_re * dz_im + dc_im;
        dz_re = new_dz_re;
        dz_im = new_dz_im;

        const D z_re = Or[m + 1] + dz_re;
        const D z_im = Oi[m + 1] + dz_im;
        const D mag2 = z_re * z_re + z_im * z_im;

        if constexpr (TrackMinMax) {
            if (mag2 < min_mag2) min_mag2 = mag2;
            if (mag2 > max_mag2) max_mag2 = mag2;
        }

        if (mag2 > bail2) {
            out.iter = n;
            out.escaped = true;
            out.escape_mag2 = static_cast<double>(mag2);
            if constexpr (TrackMinMax) {
                out.min_mag2 = static_cast<double>(min_mag2);
                out.max_mag2 = static_cast<double>(max_mag2);
            }
            return out;
        }

        ++m;
        const D dz_mag2 = dz_re * dz_re + dz_im * dz_im;
        if (mag2 < dz_mag2 || m + 1 >= olen) {
            // Rebase: the full orbit is closer to K_0 = 0 than to Z_m, or the
            // orbit data ends. dz <- z is exact because K_0 = 0.
            dz_re = z_re;
            dz_im = z_im;
            Or = Kr; Oi = Ki; olen = k_len;
            m = 0;
        }
    }
    if constexpr (TrackMinMax) {
        out.min_mag2 = static_cast<double>(min_mag2);
        out.max_mag2 = static_cast<double>(max_mag2);
    }
    return out;
}

MapStats render_map_perturbation(const MapParams& p, cv::Mat& out);

MapStats render_map_field_perturbation(const MapParams& p, FieldOutput& fo);

bool perturbation_applicable(const MapParams& p);

} // namespace fsd::compute
