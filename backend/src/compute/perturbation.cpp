// compute/perturbation.cpp
//
// Perturbation-theory renderer for deep-zoom Mandelbrot / Julia z^2 + c.
//
// Algorithm:
//   1. Compute reference orbits at high precision (double / __float128 /
//      MPFR by scale), storing each step as double. Mandelbrot needs one
//      orbit (viewport center); Julia additionally needs the critical orbit
//      of c_julia as the rebase target.
//   2. For each pixel, iterate fp64 deltas against the reference:
//      delta_z_{n+1} = 2 * Z_m * delta_z_n + delta_z_n^2 + delta_c
//      with Zhuoran rebasing (see perturbation.hpp) — glitch-free with no
//      correction passes.
//   3. Escape check uses the reconstructed full orbit: z = Z_m + delta_z.

#include "perturbation.hpp"
#include "perturbation_avx2.hpp"
#include "perturbation_avx512.hpp"
#include "colorize.hpp"
#include "colormap.hpp"
#include "parallel.hpp"
#include "scalar.hpp"

#if defined(HAS_CUDA_KERNEL)
#  include "cuda/map_kernel.cuh"      // cuda_available()
#  include "cuda/perturb_kernel.cuh"
#endif

#ifdef _OPENMP
#  include <omp.h>
#endif

#if defined(FSD_HAS_FLOAT128)
#  include <quadmath.h>
#endif

#if defined(FSD_HAS_MPFR)
#  include <mpfr.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace fsd::compute {

namespace {

// Reference-orbit caches. Within one video export, the same (center,
// iterations, bailout[, julia_c]) reference orbit is independently
// recomputed by the final-frame render, the ln-map hist_eq/bands/frontier
// stats pass, and every video segment's own ln-map render — all sharing the
// same underlying center point. Cache the most-precise orbit computed so far
// (smallest `scale` tier requested) and reuse it whenever a later call needs
// the same or less precision; recompute only when a strictly deeper scale is
// requested. Single-entry each, mirroring ln_map.cpp's g_ln_field_cache.
struct PlainOrbitCacheEntry {
    bool valid = false;
    double re = 0.0, im = 0.0;
    int max_iter = 0;
    double bailout_sq = 0.0;
    RefOrbit orbit;
};
std::mutex g_plain_orbit_cache_mu;
PlainOrbitCacheEntry g_plain_orbit_cache;

struct AutoOrbitCacheEntry {
    bool valid = false;
    std::string re_str, im_str;
    int max_iter = 0;
    double bailout_sq = 0.0;
    double scale = 0.0;
    RefOrbit orbit;
};
std::mutex g_mandel_auto_orbit_cache_mu;
AutoOrbitCacheEntry g_mandel_auto_orbit_cache;

struct JuliaAutoOrbitCacheEntry {
    bool valid = false;
    std::string re_str, im_str;
    double julia_re = 0.0, julia_im = 0.0;
    int max_iter = 0;
    double bailout_sq = 0.0;
    double scale = 0.0;
    RefOrbit orbit;
};
std::mutex g_julia_auto_orbit_cache_mu;
JuliaAutoOrbitCacheEntry g_julia_auto_orbit_cache;

} // namespace

// ---------------------------------------------------------------------------
// Reference orbit computation at __float128 precision
// ---------------------------------------------------------------------------

static RefOrbit compute_reference_orbit_impl(
    double center_re, double center_im,
    int max_iter, double bailout_sq)
{
    RefOrbit ref;
    ref.bail2 = bailout_sq;
    ref.z_re.reserve(max_iter + 1);
    ref.z_im.reserve(max_iter + 1);

#if defined(FSD_HAS_FLOAT128)
    __float128 zr = (__float128)0.0;
    __float128 zi = (__float128)0.0;
    const __float128 cr = (__float128)center_re;
    const __float128 ci = (__float128)center_im;
    const __float128 b2 = (__float128)bailout_sq;
    const __float128 two = (__float128)2.0;

    ref.z_re.push_back(0.0);
    ref.z_im.push_back(0.0);

    for (int n = 0; n < max_iter; ++n) {
        const __float128 zr2 = zr * zr;
        const __float128 zi2 = zi * zi;
        if (zr2 + zi2 > b2) {
            ref.escaped = true;
            ref.length  = n;
            return ref;
        }
        const __float128 new_zr = zr2 - zi2 + cr;
        const __float128 new_zi = two * zr * zi + ci;
        zr = new_zr;
        zi = new_zi;
        ref.z_re.push_back(static_cast<double>(zr));
        ref.z_im.push_back(static_cast<double>(zi));
    }
    ref.length  = max_iter;
    ref.escaped = false;
#else
    double zr = 0.0;
    double zi = 0.0;

    ref.z_re.push_back(0.0);
    ref.z_im.push_back(0.0);

    for (int n = 0; n < max_iter; ++n) {
        const double zr2 = zr * zr;
        const double zi2 = zi * zi;
        if (zr2 + zi2 > bailout_sq) {
            ref.escaped = true;
            ref.length  = n;
            return ref;
        }
        const double new_zr = zr2 - zi2 + center_re;
        const double new_zi = 2.0 * zr * zi + center_im;
        zr = new_zr;
        zi = new_zi;
        ref.z_re.push_back(zr);
        ref.z_im.push_back(zi);
    }
    ref.length  = max_iter;
    ref.escaped = false;
#endif

    return ref;
}

RefOrbit compute_reference_orbit(
    double center_re, double center_im,
    int max_iter, double bailout_sq)
{
    {
        std::lock_guard<std::mutex> lk(g_plain_orbit_cache_mu);
        const auto& c = g_plain_orbit_cache;
        if (c.valid && c.re == center_re && c.im == center_im &&
            c.max_iter == max_iter && c.bailout_sq == bailout_sq) {
            return c.orbit;
        }
    }
    RefOrbit ref = compute_reference_orbit_impl(center_re, center_im, max_iter, bailout_sq);
    {
        std::lock_guard<std::mutex> lk(g_plain_orbit_cache_mu);
        g_plain_orbit_cache = PlainOrbitCacheEntry{true, center_re, center_im, max_iter, bailout_sq, ref};
    }
    return ref;
}

// ---------------------------------------------------------------------------
// MPFR reference orbit for ultra-deep zoom (scale < 1e-33)
// ---------------------------------------------------------------------------

#if defined(FSD_HAS_MPFR)
namespace {

struct MpfrGuard {
    mpfr_t v;
    explicit MpfrGuard(mpfr_prec_t prec) { mpfr_init2(v, prec); }
    ~MpfrGuard() { mpfr_clear(v); }
    MpfrGuard(const MpfrGuard&) = delete;
    MpfrGuard& operator=(const MpfrGuard&) = delete;
    operator mpfr_ptr() { return v; }
};

} // namespace

static RefOrbit compute_reference_orbit_mpfr(
    const std::string& cre_str, const std::string& cim_str,
    int max_iter, double bailout_sq, int precision_bits)
{
    RefOrbit ref;
    ref.bail2 = bailout_sq;
    ref.z_re.reserve(max_iter + 1);
    ref.z_im.reserve(max_iter + 1);

    const mpfr_prec_t prec = static_cast<mpfr_prec_t>(precision_bits);
    MpfrGuard zr(prec), zi(prec), cr(prec), ci(prec);
    MpfrGuard zr2(prec), zi2(prec), tmp(prec), mag(prec), b2(prec);

    mpfr_set_d(zr, 0.0, MPFR_RNDN);
    mpfr_set_d(zi, 0.0, MPFR_RNDN);
    mpfr_set_str(cr, cre_str.c_str(), 10, MPFR_RNDN);
    mpfr_set_str(ci, cim_str.c_str(), 10, MPFR_RNDN);
    mpfr_set_d(b2, bailout_sq, MPFR_RNDN);

    ref.z_re.push_back(0.0);
    ref.z_im.push_back(0.0);

    for (int n = 0; n < max_iter; ++n) {
        mpfr_mul(zr2, zr, zr, MPFR_RNDN);  // zr²
        mpfr_mul(zi2, zi, zi, MPFR_RNDN);  // zi²
        mpfr_add(mag, zr2, zi2, MPFR_RNDN);
        if (mpfr_cmp(mag, b2) > 0) {
            ref.escaped = true;
            ref.length  = n;
            return ref;
        }
        // new_zr = zr² - zi² + cr
        mpfr_sub(tmp, zr2, zi2, MPFR_RNDN);
        mpfr_add(tmp, tmp, cr, MPFR_RNDN);
        // new_zi = 2·zr·zi + ci
        mpfr_mul(zi, zr, zi, MPFR_RNDN);
        mpfr_mul_ui(zi, zi, 2, MPFR_RNDN);
        mpfr_add(zi, zi, ci, MPFR_RNDN);
        mpfr_set(zr, tmp, MPFR_RNDN);

        ref.z_re.push_back(mpfr_get_d(zr, MPFR_RNDN));
        ref.z_im.push_back(mpfr_get_d(zi, MPFR_RNDN));
    }
    ref.length  = max_iter;
    ref.escaped = false;
    return ref;
}
#endif // FSD_HAS_MPFR

// ---------------------------------------------------------------------------
// Precision-tiered dispatch: double → __float128 → MPFR
// ---------------------------------------------------------------------------

static RefOrbit compute_reference_orbit_auto_impl(
    const std::string& cre_str, const std::string& cim_str,
    int max_iter, double bailout_sq, double scale)
{
    // If the UI supplied decimal center strings, keep them through the reference
    // orbit. Perturbation starts at scale < 1e-13, where rounding the center to
    // double can already move the reference by multiple screen pixels.
    if (cre_str.empty() || cim_str.empty()) {
        const double cre = cre_str.empty() ? 0.0 : std::stod(cre_str);
        const double cim = cim_str.empty() ? 0.0 : std::stod(cim_str);
        return compute_reference_orbit(cre, cim, max_iter, bailout_sq);
    }

    // Need more than double precision for the center. Use __float128 when it has
    // enough range for the requested scale; use MPFR below that when available.

    // Tier 2: scale ≥ 1e-33, or unknown/non-positive scale → __float128 center.
#if defined(FSD_HAS_FLOAT128)
#  if defined(FSD_HAS_MPFR)
    const bool use_fp128_tier = !(scale > 0.0) || scale >= 1e-33;
#  else
    // Without MPFR, __float128 is the deepest tier there is. Keep using it
    // below its exact range — the center rounds by ≤ ~2e-34 (sub-frame down
    // to ~1e-36) instead of collapsing to the double fallback, which renders
    // a point ~1e17 frame-widths away.
    const bool use_fp128_tier = true;
#  endif
    if (use_fp128_tier) {
        const __float128 cr = strtoflt128(cre_str.c_str(), nullptr);
        const __float128 ci = strtoflt128(cim_str.c_str(), nullptr);
        const __float128 b2 = static_cast<__float128>(bailout_sq);
        RefOrbit ref;
        ref.bail2 = bailout_sq;
        ref.z_re.reserve(max_iter + 1);
        ref.z_im.reserve(max_iter + 1);
        ref.z_re.push_back(0.0);
        ref.z_im.push_back(0.0);
        __float128 zr = 0, zi = 0;
        for (int n = 0; n < max_iter; ++n) {
            const __float128 zr2 = zr * zr;
            const __float128 zi2 = zi * zi;
            if (zr2 + zi2 > b2) {
                ref.escaped = true;
                ref.length  = n;
                return ref;
            }
            const __float128 new_zr = zr2 - zi2 + cr;
            const __float128 new_zi = (__float128)2.0 * zr * zi + ci;
            zr = new_zr;
            zi = new_zi;
            ref.z_re.push_back(static_cast<double>(zr));
            ref.z_im.push_back(static_cast<double>(zi));
        }
        ref.length  = max_iter;
        ref.escaped = false;
        return ref;
    }
#endif

    // Tier 3: scale < 1e-33 → MPFR with dynamic precision
#if defined(FSD_HAS_MPFR)
    {
        const double effective_scale =
            (scale > 0.0 && std::isfinite(scale)) ? scale : 1e-33;
        int bits = static_cast<int>(std::ceil(-std::log2(effective_scale))) + 64;
        if (bits < 128) bits = 128;
        return compute_reference_orbit_mpfr(cre_str, cim_str,
                                            max_iter, bailout_sq, bits);
    }
#endif

    // Fallback: parse to double, use existing path
    double cre = std::stod(cre_str);
    double cim = std::stod(cim_str);
    return compute_reference_orbit(cre, cim, max_iter, bailout_sq);
}

RefOrbit compute_reference_orbit_auto(
    const std::string& cre_str, const std::string& cim_str,
    int max_iter, double bailout_sq, double scale)
{
    const bool cacheable = !cre_str.empty() && !cim_str.empty();
    if (cacheable) {
        std::lock_guard<std::mutex> lk(g_mandel_auto_orbit_cache_mu);
        const auto& c = g_mandel_auto_orbit_cache;
        if (c.valid && c.re_str == cre_str && c.im_str == cim_str &&
            c.max_iter == max_iter && c.bailout_sq == bailout_sq &&
            c.scale <= scale) {
            return c.orbit;
        }
    }
    RefOrbit ref = compute_reference_orbit_auto_impl(cre_str, cim_str, max_iter, bailout_sq, scale);
    if (cacheable) {
        std::lock_guard<std::mutex> lk(g_mandel_auto_orbit_cache_mu);
        g_mandel_auto_orbit_cache = AutoOrbitCacheEntry{true, cre_str, cim_str, max_iter, bailout_sq, scale, ref};
    }
    return ref;
}

// ---------------------------------------------------------------------------
// Julia reference orbit: Z_0 = viewport center (high prec), c = julia_c (double)
// ---------------------------------------------------------------------------

RefOrbit compute_reference_orbit_julia(
    double z0_re, double z0_im,
    double julia_re, double julia_im,
    int max_iter, double bailout_sq)
{
    RefOrbit ref;
    ref.bail2 = bailout_sq;
    ref.z_re.reserve(max_iter + 1);
    ref.z_im.reserve(max_iter + 1);

#if defined(FSD_HAS_FLOAT128)
    __float128 zr = static_cast<__float128>(z0_re);
    __float128 zi = static_cast<__float128>(z0_im);
    const __float128 cr = static_cast<__float128>(julia_re);
    const __float128 ci = static_cast<__float128>(julia_im);
    const __float128 b2 = static_cast<__float128>(bailout_sq);

    ref.z_re.push_back(z0_re);
    ref.z_im.push_back(z0_im);

    for (int n = 0; n < max_iter; ++n) {
        const __float128 zr2 = zr * zr;
        const __float128 zi2 = zi * zi;
        if (zr2 + zi2 > b2) { ref.escaped = true; ref.length = n; return ref; }
        const __float128 new_zr = zr2 - zi2 + cr;
        const __float128 new_zi = static_cast<__float128>(2.0) * zr * zi + ci;
        zr = new_zr; zi = new_zi;
        ref.z_re.push_back(static_cast<double>(zr));
        ref.z_im.push_back(static_cast<double>(zi));
    }
    ref.length = max_iter; ref.escaped = false;
#else
    double zr = z0_re, zi = z0_im;
    ref.z_re.push_back(zr); ref.z_im.push_back(zi);
    for (int n = 0; n < max_iter; ++n) {
        const double zr2 = zr * zr, zi2 = zi * zi;
        if (zr2 + zi2 > bailout_sq) { ref.escaped = true; ref.length = n; return ref; }
        const double new_zr = zr2 - zi2 + julia_re;
        const double new_zi = 2.0 * zr * zi + julia_im;
        zr = new_zr; zi = new_zi;
        ref.z_re.push_back(zr); ref.z_im.push_back(zi);
    }
    ref.length = max_iter; ref.escaped = false;
#endif
    return ref;
}

#if defined(FSD_HAS_MPFR)
static RefOrbit compute_reference_orbit_julia_mpfr(
    const std::string& z0_re_str, const std::string& z0_im_str,
    double julia_re, double julia_im,
    int max_iter, double bailout_sq, int precision_bits)
{
    RefOrbit ref;
    ref.bail2 = bailout_sq;
    ref.z_re.reserve(max_iter + 1);
    ref.z_im.reserve(max_iter + 1);

    const mpfr_prec_t prec = static_cast<mpfr_prec_t>(precision_bits);
    MpfrGuard zr(prec), zi(prec), cr(prec), ci(prec);
    MpfrGuard zr2(prec), zi2(prec), tmp(prec), mag(prec), b2(prec);

    mpfr_set_str(zr, z0_re_str.c_str(), 10, MPFR_RNDN);
    mpfr_set_str(zi, z0_im_str.c_str(), 10, MPFR_RNDN);
    mpfr_set_d(cr, julia_re, MPFR_RNDN);
    mpfr_set_d(ci, julia_im, MPFR_RNDN);
    mpfr_set_d(b2, bailout_sq, MPFR_RNDN);

    ref.z_re.push_back(mpfr_get_d(zr, MPFR_RNDN));
    ref.z_im.push_back(mpfr_get_d(zi, MPFR_RNDN));

    for (int n = 0; n < max_iter; ++n) {
        mpfr_mul(zr2, zr, zr, MPFR_RNDN);
        mpfr_mul(zi2, zi, zi, MPFR_RNDN);
        mpfr_add(mag, zr2, zi2, MPFR_RNDN);
        if (mpfr_cmp(mag, b2) > 0) { ref.escaped = true; ref.length = n; return ref; }
        mpfr_sub(tmp, zr2, zi2, MPFR_RNDN);
        mpfr_add(tmp, tmp, cr, MPFR_RNDN);
        mpfr_mul(zi, zr, zi, MPFR_RNDN);
        mpfr_mul_ui(zi, zi, 2, MPFR_RNDN);
        mpfr_add(zi, zi, ci, MPFR_RNDN);
        mpfr_set(zr, tmp, MPFR_RNDN);
        ref.z_re.push_back(mpfr_get_d(zr, MPFR_RNDN));
        ref.z_im.push_back(mpfr_get_d(zi, MPFR_RNDN));
    }
    ref.length = max_iter; ref.escaped = false;
    return ref;
}
#endif

static RefOrbit compute_reference_orbit_julia_auto_impl(
    const std::string& z0_re_str, const std::string& z0_im_str,
    double julia_re, double julia_im,
    int max_iter, double bailout_sq, double scale)
{
    if (z0_re_str.empty() || z0_im_str.empty()) {
        const double z0re = z0_re_str.empty() ? 0.0 : std::stod(z0_re_str);
        const double z0im = z0_im_str.empty() ? 0.0 : std::stod(z0_im_str);
        return compute_reference_orbit_julia(z0re, z0im, julia_re, julia_im,
                                             max_iter, bailout_sq);
    }

#if defined(FSD_HAS_FLOAT128)
#  if defined(FSD_HAS_MPFR)
    const bool use_fp128_tier = !(scale > 0.0) || scale >= 1e-33;
#  else
    const bool use_fp128_tier = true;   // deepest available tier (see above)
#  endif
    if (use_fp128_tier) {
        const __float128 zr0 = strtoflt128(z0_re_str.c_str(), nullptr);
        const __float128 zi0 = strtoflt128(z0_im_str.c_str(), nullptr);
        const __float128 cr = static_cast<__float128>(julia_re);
        const __float128 ci = static_cast<__float128>(julia_im);
        const __float128 b2 = static_cast<__float128>(bailout_sq);
        RefOrbit ref;
        ref.bail2 = bailout_sq;
        ref.z_re.reserve(max_iter + 1);
        ref.z_im.reserve(max_iter + 1);
        __float128 zr = zr0, zi = zi0;
        ref.z_re.push_back(static_cast<double>(zr));
        ref.z_im.push_back(static_cast<double>(zi));
        for (int n = 0; n < max_iter; ++n) {
            const __float128 zr2 = zr * zr, zi2 = zi * zi;
            if (zr2 + zi2 > b2) { ref.escaped = true; ref.length = n; return ref; }
            const __float128 new_zr = zr2 - zi2 + cr;
            const __float128 new_zi = static_cast<__float128>(2.0) * zr * zi + ci;
            zr = new_zr; zi = new_zi;
            ref.z_re.push_back(static_cast<double>(zr));
            ref.z_im.push_back(static_cast<double>(zi));
        }
        ref.length = max_iter; ref.escaped = false;
        return ref;
    }
#endif

#if defined(FSD_HAS_MPFR)
    {
        const double effective_scale =
            (scale > 0.0 && std::isfinite(scale)) ? scale : 1e-33;
        int bits = static_cast<int>(std::ceil(-std::log2(effective_scale))) + 64;
        if (bits < 128) bits = 128;
        return compute_reference_orbit_julia_mpfr(z0_re_str, z0_im_str,
                                                   julia_re, julia_im,
                                                   max_iter, bailout_sq, bits);
    }
#endif

    const double z0re = std::stod(z0_re_str);
    const double z0im = std::stod(z0_im_str);
    return compute_reference_orbit_julia(z0re, z0im, julia_re, julia_im,
                                         max_iter, bailout_sq);
}

RefOrbit compute_reference_orbit_julia_auto(
    const std::string& z0_re_str, const std::string& z0_im_str,
    double julia_re, double julia_im,
    int max_iter, double bailout_sq, double scale)
{
    const bool cacheable = !z0_re_str.empty() && !z0_im_str.empty();
    if (cacheable) {
        std::lock_guard<std::mutex> lk(g_julia_auto_orbit_cache_mu);
        const auto& c = g_julia_auto_orbit_cache;
        if (c.valid && c.re_str == z0_re_str && c.im_str == z0_im_str &&
            c.julia_re == julia_re && c.julia_im == julia_im &&
            c.max_iter == max_iter && c.bailout_sq == bailout_sq &&
            c.scale <= scale) {
            return c.orbit;
        }
    }
    RefOrbit ref = compute_reference_orbit_julia_auto_impl(
        z0_re_str, z0_im_str, julia_re, julia_im, max_iter, bailout_sq, scale);
    if (cacheable) {
        std::lock_guard<std::mutex> lk(g_julia_auto_orbit_cache_mu);
        g_julia_auto_orbit_cache = JuliaAutoOrbitCacheEntry{
            true, z0_re_str, z0_im_str, julia_re, julia_im, max_iter, bailout_sq, scale, ref};
    }
    return ref;
}

// ---------------------------------------------------------------------------
// Check if perturbation is applicable for this request
// ---------------------------------------------------------------------------

bool perturbation_applicable(const MapParams& p)
{
    if (p.variant != Variant::Mandelbrot) return false;
    // MinPairwiseDist needs every orbit point per pixel (O(n^2) pairwise
    // scan). MandelShipAgree is not a Mandelbrot orbit metric at all: it
    // compares the selected variant's orbit against z^2+c and needs the
    // selected variant step to run explicitly.
    if (p.metric == Metric::MinPairwiseDist ||
        p.metric == Metric::MandelShipAgree) return false;
    if (p.custom_step_fn)                  return false;
    if (p.scale >= 1e-13)                  return false;
    if (p.scalar_type != "auto" && p.scalar_type != "perturbation")
        return false;
    return true;
}

// ---------------------------------------------------------------------------
// Shared OpenMP driver.
//
// Runs the perturbation render and reports each finished pixel through
// `emit(px_idx, x, y, PerturbPixel)` (called once per pixel, from parallel
// regions, distinct pixels only). The BGR and field renderers below differ
// only in what they do with a finished pixel.
// ---------------------------------------------------------------------------

namespace {

template <typename Emit>
MapStats run_perturbation_driver(const MapParams& p, Emit&& emit)
{
    const auto t0 = std::chrono::steady_clock::now();

    const int W = p.width;
    const int H = p.height;
    const double aspect  = static_cast<double>(W) / H;
    const double span_im = p.scale;
    const double span_re = p.scale * aspect;
    const double bail2   = p.bailout_sq;
    const int    max_iter = p.iterations;
    const int    thread_count = resolve_render_threads(p.render_threads);

    const bool has_rot = p.rotation_deg != 0.0;
    const double rot_rad = has_rot ? p.rotation_deg * M_PI / 180.0 : 0.0;
    const double cos_t = has_rot ? std::cos(rot_rad) : 1.0;
    const double sin_t = has_rot ? std::sin(rot_rad) : 0.0;

    const bool is_julia = p.julia;

    // Primary reference orbit at the viewport center; for Julia additionally
    // the critical orbit of c_julia as the rebase target (see header).
    RefOrbit ref;
    RefOrbit crit;
    if (is_julia) {
        ref = p.center_re_str.empty()
            ? compute_reference_orbit_julia(p.center_re, p.center_im,
                                             p.julia_re, p.julia_im, max_iter, bail2)
            : compute_reference_orbit_julia_auto(p.center_re_str, p.center_im_str,
                                                  p.julia_re, p.julia_im,
                                                  max_iter, bail2, p.scale);
        crit = compute_reference_orbit(p.julia_re, p.julia_im, max_iter, bail2);
    } else {
        ref = p.center_re_str.empty()
            ? compute_reference_orbit(p.center_re, p.center_im, max_iter, bail2)
            : compute_reference_orbit_auto(p.center_re_str, p.center_im_str,
                                           max_iter, bail2, p.scale);
    }
    const RefOrbit& kref = is_julia ? crit : ref;

    const double* Rr = ref.z_re.data();
    const double* Ri = ref.z_im.data();
    const int     Rlen = static_cast<int>(ref.z_re.size());
    const double* Kr = kref.z_re.data();
    const double* Ki = kref.z_im.data();
    const int     Klen = static_cast<int>(kref.z_re.size());

    // Combined orbit table for the batch kernels (AVX2/CUDA): R then K.
    // Mandelbrot aliases both windows onto the same orbit without copying.
    std::vector<double> tab_re_store, tab_im_store;
    const double* tab_re = Rr;
    const double* tab_im = Ri;
    int tab_len = Rlen;
    int k_off = 0;
    if (is_julia) {
        tab_re_store.reserve(ref.z_re.size() + kref.z_re.size());
        tab_im_store.reserve(ref.z_im.size() + kref.z_im.size());
        tab_re_store.assign(ref.z_re.begin(), ref.z_re.end());
        tab_re_store.insert(tab_re_store.end(), kref.z_re.begin(), kref.z_re.end());
        tab_im_store.assign(ref.z_im.begin(), ref.z_im.end());
        tab_im_store.insert(tab_im_store.end(), kref.z_im.begin(), kref.z_im.end());
        tab_re = tab_re_store.data();
        tab_im = tab_im_store.data();
        tab_len = Rlen + Klen;
        k_off = Rlen;
    }

    // Degenerate primary orbit (seed escaped instantly): batch kernels start
    // pixels on K with dz pre-shifted by R_0 — same as perturb_iterate's guard.
    int start_off = 0, start_len = Rlen;
    double dz_shift_re = 0.0, dz_shift_im = 0.0;
    if (Rlen < 2) {
        dz_shift_re = Rr[0];
        dz_shift_im = Ri[0];
        start_off = k_off;
        start_len = Klen;
    }
    const bool batch_ok = start_len >= 2 && Klen >= 2;

    // Backend: explicit engine request, or auto → AVX-512, CUDA, AVX2, scalar
    // (matching select_map_engine's no-benchmark preference order).
    std::string want = p.engine;
    if (want == "auto") {
        if (perturb_avx512_available()) want = "avx512";
#if defined(HAS_CUDA_KERNEL)
        else if (fsd_cuda::cuda_available()) want = "cuda";
#endif
        else if (perturb_avx2_available()) want = "avx2";
        else want = "openmp";
    }
    [[maybe_unused]] const bool wants_cuda = want == "cuda" || want == "hybrid";
    using BatchFn = void (*)(
        const double*, const double*, int, int, int, int,
        const double*, const double*, const double*, const double*,
        int, int, double, int32_t*, double*) noexcept;
    // Min/max metrics run scalar-only: the batch kernels track escape state
    // per lane but not orbit extrema.
    const bool track_minmax = p.metric != Metric::Escape;

    // Best CPU batch kernel — the primary path for the AVX engines and the
    // fallback when a CUDA render fails.
    BatchFn batch = nullptr;
    const char* batch_engine = "openmp";
    if (batch_ok && !track_minmax && want != "openmp") {
        if (want != "avx2" && perturb_avx512_available()) {
            batch = perturb_iterate_batch_avx512;
            batch_engine = "avx512";
        } else if (perturb_avx2_available()) {
            batch = perturb_iterate_batch_avx2;
            batch_engine = "avx2";
        }
    }
    std::string engine_used = "openmp";
    bool rendered = false;

#if defined(HAS_CUDA_KERNEL)
    if (!rendered && batch_ok && !track_minmax && wants_cuda && fsd_cuda::cuda_available()) {
        if (map_render_cancel_requested(p)) throw std::runtime_error("cancelled");
        fsd_cuda::CudaPerturbParams cp;
        cp.width = W; cp.height = H; cp.iterations = max_iter;
        cp.bailout_sq = bail2;
        cp.offset_mode = 0;
        cp.span_re = span_re; cp.span_im = span_im;
        cp.cos_t = cos_t; cp.sin_t = sin_t;
        cp.julia = is_julia;
        cp.dz_shift_re = dz_shift_re; cp.dz_shift_im = dz_shift_im;
        cp.tab_re = tab_re; cp.tab_im = tab_im; cp.tab_len = tab_len;
        cp.start_off = start_off; cp.start_len = start_len;
        cp.k_off = k_off; cp.k_len = Klen;
        std::vector<uint32_t> iters(static_cast<size_t>(W) * H);
        std::vector<float> norms(static_cast<size_t>(W) * H);
        if (fsd_cuda::cuda_render_perturb_field(cp, iters.data(), norms.data(), nullptr)) {
            #pragma omp parallel for num_threads(thread_count) schedule(static)
            for (int idx = 0; idx < W * H; ++idx) {
                PerturbPixel res;
                res.iter = static_cast<int>(iters[idx]);
                res.escaped = res.iter < max_iter;
                res.escape_mag2 = norms[idx];
                emit(static_cast<size_t>(idx), idx % W, idx / W, res);
            }
            engine_used = "cuda";
            rendered = true;
        }
    }
#endif

    if (!rendered) {
        constexpr int tile_size = 32;
        const int tiles_x = (W + tile_size - 1) / tile_size;
        const int tiles_y = (H + tile_size - 1) / tile_size;
        const int tile_count = tiles_x * tiles_y;
        std::atomic<bool> cancelled{false};

        #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 1)
        for (int tile = 0; tile < tile_count; ++tile) {
            if (cancelled.load(std::memory_order_relaxed)) continue;
            if (map_render_cancel_requested(p)) {
                cancelled.store(true, std::memory_order_relaxed);
                continue;
            }
            const int tx = tile % tiles_x;
            const int ty = tile / tiles_x;
            const int x0 = tx * tile_size;
            const int y0 = ty * tile_size;
            const int x1 = std::min(W, x0 + tile_size);
            const int y1 = std::min(H, y0 + tile_size);

            auto pixel_offset = [&](int x, int y, double& out_re, double& out_im) {
                const double dx = span_re * ((static_cast<double>(x) + 0.5) / W - 0.5);
                const double dy = -span_im * ((static_cast<double>(y) + 0.5) / H - 0.5);
                if (has_rot) {
                    out_re = dx * cos_t - dy * sin_t;
                    out_im = dx * sin_t + dy * cos_t;
                } else {
                    out_re = dx;
                    out_im = dy;
                }
            };

            if (batch) {
                alignas(64) double bdz_re[tile_size * tile_size];
                alignas(64) double bdz_im[tile_size * tile_size];
                alignas(64) double bdc_re[tile_size * tile_size];
                alignas(64) double bdc_im[tile_size * tile_size];
                int32_t b_iter[tile_size * tile_size];
                double  b_mag2[tile_size * tile_size];

                int cnt = 0;
                for (int y = y0; y < y1; ++y) {
                    for (int x = x0; x < x1; ++x, ++cnt) {
                        double off_re, off_im;
                        pixel_offset(x, y, off_re, off_im);
                        if (is_julia) {
                            bdz_re[cnt] = off_re + dz_shift_re;
                            bdz_im[cnt] = off_im + dz_shift_im;
                            bdc_re[cnt] = 0.0;
                            bdc_im[cnt] = 0.0;
                        } else {
                            bdz_re[cnt] = dz_shift_re;
                            bdz_im[cnt] = dz_shift_im;
                            bdc_re[cnt] = off_re;
                            bdc_im[cnt] = off_im;
                        }
                    }
                }
                batch(tab_re, tab_im, start_off, start_len, k_off, Klen,
                      bdz_re, bdz_im, bdc_re, bdc_im,
                      cnt, max_iter, bail2, b_iter, b_mag2);

                int j = 0;
                for (int y = y0; y < y1; ++y) {
                    for (int x = x0; x < x1; ++x, ++j) {
                        PerturbPixel res;
                        res.iter = b_iter[j];
                        res.escaped = res.iter < max_iter;
                        res.escape_mag2 = b_mag2[j];
                        emit(static_cast<size_t>(y) * W + x, x, y, res);
                    }
                }
            } else {
                for (int y = y0; y < y1; ++y) {
                    for (int x = x0; x < x1; ++x) {
                        double off_re, off_im;
                        pixel_offset(x, y, off_re, off_im);

                        // Julia: pixel offset enters as delta_z0, c is shared
                        // exactly. Mandelbrot: pixel offset enters as delta_c.
                        const double dz0_re = is_julia ? off_re : 0.0;
                        const double dz0_im = is_julia ? off_im : 0.0;
                        const double dc_re  = is_julia ? 0.0 : off_re;
                        const double dc_im  = is_julia ? 0.0 : off_im;

                        const PerturbPixel res = track_minmax
                            ? perturb_iterate<true>(
                                  Rr, Ri, Rlen, Kr, Ki, Klen,
                                  dz0_re, dz0_im, dc_re, dc_im, max_iter, bail2)
                            : perturb_iterate<false>(
                                  Rr, Ri, Rlen, Kr, Ki, Klen,
                                  dz0_re, dz0_im, dc_re, dc_im, max_iter, bail2);

                        emit(static_cast<size_t>(y) * W + x, x, y, res);
                    }
                }
            }
        }

        if (cancelled.load(std::memory_order_relaxed)) {
            throw std::runtime_error("cancelled");
        }
        engine_used = batch_engine;
    }

    const auto t1 = std::chrono::steady_clock::now();
    MapStats stats;
    stats.elapsed_ms  = std::chrono::duration<double, std::milli>(t1 - t0).count();
    stats.pixel_count = W * H;
    stats.scalar_used = "perturbation_fp64";
    stats.engine_used = engine_used;
    return stats;
}

} // namespace

// ---------------------------------------------------------------------------
// Public renderers: BGR image and raw field (iter counts + |z|² at escape)
// ---------------------------------------------------------------------------

MapStats render_map_perturbation(const MapParams& p, cv::Mat& out)
{
    if (p.metric != Metric::Escape) {
        // Metric fields share the coloring pipeline with the plain engines.
        FieldOutput fo;
        MapStats stats = render_map_field_perturbation(p, fo);
        out = colorize_direct(p, fo);
        return stats;
    }
    const int max_iter = p.iterations;
    return run_perturbation_driver(p,
        [&](size_t, int x, int y, const PerturbPixel& res) {
            uint8_t* px = out.ptr<uint8_t>(y) + 3 * x;
            colorize_escape_bgr(res.iter, max_iter, p.colormap,
                                res.escape_mag2, p.smooth, px[0], px[1], px[2]);
        });
}

MapStats render_map_field_perturbation(const MapParams& p, FieldOutput& fo)
{
    const size_t px_count = static_cast<size_t>(p.width) * p.height;
    fo.width  = p.width;
    fo.height = p.height;
    fo.metric = p.metric;

    const bool is_escape = p.metric == Metric::Escape;
    if (is_escape) {
        fo.iter_u32.assign(px_count, 0u);
        fo.norm_f32.assign(px_count, 0.0f);
    } else {
        fo.field_f64.assign(px_count, 0.0);
    }

    MapStats stats = run_perturbation_driver(p,
        [&](size_t px_idx, int, int, const PerturbPixel& res) {
            if (is_escape) {
                fo.iter_u32[px_idx] = static_cast<uint32_t>(res.iter);
                fo.norm_f32[px_idx] = res.escaped ? static_cast<float>(res.escape_mag2) : 0.0f;
                return;
            }
            // Mirror iterate_quadratic_cached_masked's finalize +
            // raw_field_value: sqrt at the end, unset extrema map to 0.
            const double min_abs = std::sqrt(res.min_mag2);   // inf stays inf
            const double max_abs = res.max_mag2 > 0.0 ? std::sqrt(res.max_mag2) : 0.0;
            double v = 0.0;
            switch (p.metric) {
                case Metric::MinAbs:
                    v = std::isfinite(min_abs) ? min_abs : 0.0;
                    break;
                case Metric::MaxAbs:
                    v = max_abs;
                    break;
                case Metric::Envelope:
                    v = std::isfinite(min_abs) ? 0.5 * (min_abs + max_abs) : 0.0;
                    break;
                default:
                    break;
            }
            fo.field_f64[px_idx] = v;
        });

    if (!is_escape) {
        double lo =  std::numeric_limits<double>::infinity();
        double hi = -std::numeric_limits<double>::infinity();
        for (double v : fo.field_f64) {
            if (std::isfinite(v)) {
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
        }
        fo.field_min = std::isfinite(lo) ? lo : 0.0;
        fo.field_max = std::isfinite(hi) ? hi : 1.0;
    }

    fo.scalar_used = stats.scalar_used;
    fo.engine_used = stats.engine_used;
    return stats;
}

} // namespace fsd::compute
