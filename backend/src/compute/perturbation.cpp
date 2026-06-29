// compute/perturbation.cpp
//
// Perturbation-theory renderer for deep-zoom Mandelbrot z^2 + c.
//
// Algorithm:
//   1. Compute a single reference orbit Z_n at the viewport center using
//      __float128 precision, storing each step as double.
//   2. For each pixel, compute delta_c = c_pixel - c_ref in fp64.
//      Iterate: delta_z_{n+1} = 2 * Z_n * delta_z_n + delta_z_n^2 + delta_c
//   3. Escape check uses the reconstructed full orbit: z = Z_n + delta_z_n.
//   4. Glitch detection: when |delta_z| becomes too large relative to |Z_n|,
//      the pixel loses significance. Mark as "glitch" and rebase later.

#include "perturbation.hpp"
#include "colormap.hpp"
#include "parallel.hpp"
#include "scalar.hpp"

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
#include <string>
#include <vector>

namespace fsd::compute {

namespace {

constexpr double GLITCH_TOLERANCE = 1e-3;

} // namespace

// ---------------------------------------------------------------------------
// Reference orbit computation at __float128 precision
// ---------------------------------------------------------------------------

RefOrbit compute_reference_orbit(
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

RefOrbit compute_reference_orbit_auto(
    const std::string& cre_str, const std::string& cim_str,
    int max_iter, double bailout_sq, double scale)
{
    // Tier 1: scale ≥ 1e-15 → double center is sufficient (fp64 has ~15 digits).
    // __float128 iteration is still used when available (via compute_reference_orbit).
    if (scale >= 1e-15 || cre_str.empty()) {
        double cre = cre_str.empty() ? 0.0 : std::stod(cre_str);
        double cim = cim_str.empty() ? 0.0 : std::stod(cim_str);
        return compute_reference_orbit(cre, cim, max_iter, bailout_sq);
    }

    // Between 1e-15 and 1e-33: need more than double precision for the center.
    // Fall through to Tier 2 (__float128 with strtoflt128) or Tier 3 (MPFR).

    // Tier 2: scale ≥ 1e-33 → __float128 iteration with string-parsed center
#if defined(FSD_HAS_FLOAT128)
    if (scale >= 1e-33) {
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
        int bits = static_cast<int>(std::ceil(-std::log2(scale))) + 64;
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

RefOrbit compute_reference_orbit_julia_auto(
    const std::string& z0_re_str, const std::string& z0_im_str,
    double julia_re, double julia_im,
    int max_iter, double bailout_sq, double scale)
{
    if (scale >= 1e-15 || z0_re_str.empty()) {
        double z0re = z0_re_str.empty() ? 0.0 : std::stod(z0_re_str);
        double z0im = z0_im_str.empty() ? 0.0 : std::stod(z0_im_str);
        return compute_reference_orbit_julia(z0re, z0im, julia_re, julia_im,
                                             max_iter, bailout_sq);
    }

#if defined(FSD_HAS_FLOAT128)
    if (scale >= 1e-33) {
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
        int bits = static_cast<int>(std::ceil(-std::log2(scale))) + 64;
        if (bits < 128) bits = 128;
        return compute_reference_orbit_julia_mpfr(z0_re_str, z0_im_str,
                                                   julia_re, julia_im,
                                                   max_iter, bailout_sq, bits);
    }
#endif

    double z0re = std::stod(z0_re_str);
    double z0im = std::stod(z0_im_str);
    return compute_reference_orbit_julia(z0re, z0im, julia_re, julia_im,
                                         max_iter, bailout_sq);
}

// ---------------------------------------------------------------------------
// Check if perturbation is applicable for this request
// ---------------------------------------------------------------------------

bool perturbation_applicable(const MapParams& p)
{
    if (p.variant != Variant::Mandelbrot) return false;
    if (p.metric  != Metric::Escape)      return false;
    if (p.smooth)                          return false;
    if (p.custom_step_fn)                  return false;
    if (p.scale >= 1e-13)                  return false;
    if (p.scalar_type != "auto" && p.scalar_type != "perturbation")
        return false;
    return true;
}

// ---------------------------------------------------------------------------
// OpenMP perturbation renderer
// ---------------------------------------------------------------------------

MapStats render_map_perturbation(const MapParams& p, cv::Mat& out)
{
    auto t0 = std::chrono::steady_clock::now();

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

    RefOrbit ref;
    if (is_julia) {
        ref = p.center_re_str.empty()
            ? compute_reference_orbit_julia(p.center_re, p.center_im,
                                             p.julia_re, p.julia_im, max_iter, bail2)
            : compute_reference_orbit_julia_auto(p.center_re_str, p.center_im_str,
                                                  p.julia_re, p.julia_im,
                                                  max_iter, bail2, p.scale);
    } else {
        ref = p.center_re_str.empty()
            ? compute_reference_orbit(p.center_re, p.center_im, max_iter, bail2)
            : compute_reference_orbit_auto(p.center_re_str, p.center_im_str,
                                           max_iter, bail2, p.scale);
    }

    // Step 2: per-pixel perturbation iteration
    constexpr int tile_size = 32;
    const int tiles_x = (W + tile_size - 1) / tile_size;
    const int tiles_y = (H + tile_size - 1) / tile_size;
    const int tile_count = tiles_x * tiles_y;
    std::atomic<bool> cancelled{false};

    // Glitch buffer: 0 = no glitch, 1 = glitch pixel needing rebase
    std::vector<uint8_t> glitch(static_cast<size_t>(W) * H, 0);
    int total_glitches = 0;

    auto render_pass = [&](const RefOrbit& orbit, double ref_re, double ref_im,
                            const uint8_t* glitch_mask, bool only_glitched) {
        const int orbit_len = static_cast<int>(orbit.z_re.size());
        const double* Or = orbit.z_re.data();
        const double* Oi = orbit.z_im.data();

        int pass_glitches = 0;

        #pragma omp parallel num_threads(thread_count) reduction(+:pass_glitches)
        {
            #pragma omp for schedule(dynamic, 1)
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

                for (int y = y0; y < y1; ++y) {
                    uint8_t* row = out.ptr<uint8_t>(y);

                    for (int x = x0; x < x1; ++x) {
                        const size_t px_idx = static_cast<size_t>(y) * W + x;
                        if (only_glitched && !glitch_mask[px_idx]) continue;

                        double dx = span_re * ((static_cast<double>(x) + 0.5) / W - 0.5);
                        double dy = -span_im * ((static_cast<double>(y) + 0.5) / H - 0.5);
                        double rot_dx, rot_dy;
                        if (has_rot) {
                            rot_dx = dx * cos_t - dy * sin_t;
                            rot_dy = dx * sin_t + dy * cos_t;
                        } else {
                            rot_dx = dx; rot_dy = dy;
                        }
                        const double px_off_re = rot_dx + (p.center_re - ref_re);
                        const double px_off_im = rot_dy + (p.center_im - ref_im);

                        double dc_re, dc_im, dz_re, dz_im;
                        if (is_julia) {
                            dc_re = 0.0; dc_im = 0.0;
                            dz_re = px_off_re; dz_im = px_off_im;
                        } else {
                            dc_re = px_off_re; dc_im = px_off_im;
                            dz_re = 0.0; dz_im = 0.0;
                        }
                        int iter = max_iter;
                        bool escaped = false;
                        bool is_glitch = false;

                        const int limit = std::min(max_iter, orbit_len - 1);
                        for (int n = 0; n < limit; ++n) {
                            const double Zn_re = Or[n];
                            const double Zn_im = Oi[n];

                            // delta_z_{n+1} = 2 * Z_n * delta_z + delta_z^2 + delta_c
                            // (Z_n + delta_z)^2 + c = Z_n^2 + 2*Z_n*dz + dz^2 + c
                            //                       = Z_{n+1} + 2*Z_n*dz + dz^2 + dc
                            const double two_Zn_re = 2.0 * Zn_re;
                            const double two_Zn_im = 2.0 * Zn_im;

                            const double new_dz_re = two_Zn_re * dz_re - two_Zn_im * dz_im
                                                   + dz_re * dz_re - dz_im * dz_im
                                                   + dc_re;
                            const double new_dz_im = two_Zn_re * dz_im + two_Zn_im * dz_re
                                                   + 2.0 * dz_re * dz_im
                                                   + dc_im;

                            dz_re = new_dz_re;
                            dz_im = new_dz_im;

                            // Escape check on full orbit value
                            const double z_re = Or[n + 1] + dz_re;
                            const double z_im = Oi[n + 1] + dz_im;
                            const double mag2 = z_re * z_re + z_im * z_im;

                            if (mag2 > bail2) {
                                iter = n;
                                escaped = true;
                                break;
                            }

                            if (!std::isfinite(dz_re) || !std::isfinite(dz_im)) {
                                is_glitch = true;
                                break;
                            }

                            // Glitch detection: catastrophic cancellation when
                            // |z_full|² << |delta_z|² (Z and delta nearly cancelled).
                            const double dz_mag2 = dz_re * dz_re + dz_im * dz_im;
                            if (dz_mag2 > 1e-4 && mag2 < GLITCH_TOLERANCE * dz_mag2) {
                                is_glitch = true;
                                break;
                            }
                        }

                        if (is_glitch) {
                            glitch[px_idx] = 1;
                            ++pass_glitches;
                            continue;
                        }

                        if (!escaped && limit < max_iter) {
                            // Perturbation loop truncated by reference orbit length:
                            // pixel may need more iterations. fp128 re-check.
                            glitch[px_idx] = 2;
                            ++pass_glitches;
                            continue;
                        }

                        glitch[px_idx] = 0;
                        uint8_t* px = row + 3 * x;
                        colorize_escape_bgr(
                            escaped ? iter : max_iter,
                            max_iter, p.colormap,
                            0.0, false,
                            px[0], px[1], px[2]);
                    }
                }
            }
        }
        return pass_glitches;
    };

    // Primary pass with center reference
    total_glitches = render_pass(ref, p.center_re, p.center_im, nullptr, false);

    // Rebase passes for glitch=1 (cancellation glitches), up to 3 rounds.
    for (int rebase = 0; rebase < 3; ++rebase) {
        if (cancelled.load(std::memory_order_relaxed)) break;
        double rebase_re = p.center_re, rebase_im = p.center_im;
        bool found = false;
        for (int y = 0; y < H && !found; ++y)
            for (int x = 0; x < W && !found; ++x)
                if (glitch[static_cast<size_t>(y) * W + x] == 1) {
                    double dx = span_re * ((static_cast<double>(x) + 0.5) / W - 0.5);
                    double dy = -span_im * ((static_cast<double>(y) + 0.5) / H - 0.5);
                    if (has_rot) {
                        rebase_re = p.center_re + dx * cos_t - dy * sin_t;
                        rebase_im = p.center_im + dx * sin_t + dy * cos_t;
                    } else {
                        rebase_re = p.center_re + dx;
                        rebase_im = p.center_im + dy;
                    }
                    found = true;
                }
        if (!found) break;
        RefOrbit rebase_ref = is_julia
            ? compute_reference_orbit_julia(rebase_re, rebase_im,
                                             p.julia_re, p.julia_im, max_iter, bail2)
            : compute_reference_orbit(rebase_re, rebase_im, max_iter, bail2);
        render_pass(rebase_ref, rebase_re, rebase_im, glitch.data(), true);
    }

    // Direct high-precision fallback for remaining unresolved pixels.
    // MPFR (arbitrary precision) → __float128 → double, by zoom depth.
    {
        const int mpfr_bits = std::max(128, static_cast<int>(std::ceil(
            p.scale > 0 ? -std::log2(p.scale) : 53)) + 64);

        #pragma omp parallel num_threads(thread_count)
        {
            #pragma omp for schedule(dynamic, 16)
            for (int idx = 0; idx < W * H; ++idx) {
                if (!glitch[idx]) continue;
                const int x = idx % W;
                const int y = idx / W;
                const double frac_x = (static_cast<double>(x) + 0.5) / W - 0.5;
                const double frac_y = (static_cast<double>(y) + 0.5) / H - 0.5;
                const double dx = span_re * frac_x;
                const double dy = -span_im * frac_y;
                double off_re, off_im;
                if (has_rot) {
                    off_re = dx * cos_t - dy * sin_t;
                    off_im = dx * sin_t + dy * cos_t;
                } else {
                    off_re = dx;
                    off_im = dy;
                }
                int iter = max_iter;

#if defined(FSD_HAS_MPFR)
                {
                    const mpfr_prec_t prec = static_cast<mpfr_prec_t>(mpfr_bits);
                    MpfrGuard cr(prec), ci(prec), zr(prec), zi(prec);
                    MpfrGuard zr2(prec), zi2(prec), tmp(prec), mag(prec), b2(prec);
                    // c = center + rotated offset
                    if (!p.center_re_str.empty())
                        mpfr_set_str(cr, p.center_re_str.c_str(), 10, MPFR_RNDN);
                    else
                        mpfr_set_d(cr, p.center_re, MPFR_RNDN);
                    mpfr_set_d(tmp, off_re, MPFR_RNDN);
                    mpfr_add(cr, cr, tmp, MPFR_RNDN);
                    if (!p.center_im_str.empty())
                        mpfr_set_str(ci, p.center_im_str.c_str(), 10, MPFR_RNDN);
                    else
                        mpfr_set_d(ci, p.center_im, MPFR_RNDN);
                    mpfr_set_d(tmp, off_im, MPFR_RNDN);
                    mpfr_add(ci, ci, tmp, MPFR_RNDN);
                    mpfr_set_d(zr, 0.0, MPFR_RNDN);
                    mpfr_set_d(zi, 0.0, MPFR_RNDN);
                    mpfr_set_d(b2, bail2, MPFR_RNDN);
                    for (int n = 0; n < max_iter; ++n) {
                        mpfr_mul(zr2, zr, zr, MPFR_RNDN);
                        mpfr_mul(zi2, zi, zi, MPFR_RNDN);
                        mpfr_add(mag, zr2, zi2, MPFR_RNDN);
                        if (mpfr_cmp(mag, b2) > 0) { iter = n; break; }
                        mpfr_sub(tmp, zr2, zi2, MPFR_RNDN);
                        mpfr_add(tmp, tmp, cr, MPFR_RNDN);
                        mpfr_mul(zi, zr, zi, MPFR_RNDN);
                        mpfr_mul_ui(zi, zi, 2, MPFR_RNDN);
                        mpfr_add(zi, zi, ci, MPFR_RNDN);
                        mpfr_set(zr, tmp, MPFR_RNDN);
                    }
                }
#elif defined(FSD_HAS_FLOAT128)
                {
                    const __float128 cre = scalar_from_string<__float128>(p.center_re_str, p.center_re)
                        + static_cast<__float128>(off_re);
                    const __float128 cim = scalar_from_string<__float128>(p.center_im_str, p.center_im)
                        + static_cast<__float128>(off_im);
                    __float128 zr = 0, zi = 0;
                    for (int n = 0; n < max_iter; ++n) {
                        const __float128 zr2 = zr * zr, zi2 = zi * zi;
                        if (static_cast<double>(zr2 + zi2) > bail2) { iter = n; break; }
                        const __float128 nzi = static_cast<__float128>(2.0) * zr * zi + cim;
                        zr = zr2 - zi2 + cre;
                        zi = nzi;
                    }
                }
#else
                {
                    const double cre = p.center_re + off_re;
                    const double cim = p.center_im + off_im;
                    double zr = 0, zi = 0;
                    for (int n = 0; n < max_iter; ++n) {
                        const double zr2 = zr * zr, zi2 = zi * zi;
                        if (zr2 + zi2 > bail2) { iter = n; break; }
                        const double nzi = 2.0 * zr * zi + cim;
                        zr = zr2 - zi2 + cre;
                        zi = nzi;
                    }
                }
#endif
                uint8_t* px = out.ptr<uint8_t>(y) + 3 * x;
                colorize_escape_bgr(iter, max_iter, p.colormap, 0.0, false, px[0], px[1], px[2]);
            }
        }
    }

    if (cancelled.load(std::memory_order_relaxed)) {
        throw std::runtime_error("cancelled");
    }

    auto t1 = std::chrono::steady_clock::now();
    MapStats stats;
    stats.elapsed_ms  = std::chrono::duration<double, std::milli>(t1 - t0).count();
    stats.pixel_count = W * H;
    stats.scalar_used = "perturbation_fp64";
    stats.engine_used = "openmp";
    return stats;
}

// ---------------------------------------------------------------------------
// Perturbation field output (raw iter counts + |z|² for equalized coloring)
// ---------------------------------------------------------------------------

MapStats render_map_field_perturbation(const MapParams& p, FieldOutput& fo)
{
    auto t0 = std::chrono::steady_clock::now();

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

    fo.width  = W;
    fo.height = H;
    fo.metric = Metric::Escape;
    fo.iter_u32.assign(static_cast<size_t>(W) * H, 0u);
    fo.norm_f32.assign(static_cast<size_t>(W) * H, 0.0f);

    const bool is_julia = p.julia;

    RefOrbit ref;
    if (is_julia) {
        ref = p.center_re_str.empty()
            ? compute_reference_orbit_julia(p.center_re, p.center_im,
                                             p.julia_re, p.julia_im, max_iter, bail2)
            : compute_reference_orbit_julia_auto(p.center_re_str, p.center_im_str,
                                                  p.julia_re, p.julia_im,
                                                  max_iter, bail2, p.scale);
    } else {
        ref = p.center_re_str.empty()
            ? compute_reference_orbit(p.center_re, p.center_im, max_iter, bail2)
            : compute_reference_orbit_auto(p.center_re_str, p.center_im_str,
                                           max_iter, bail2, p.scale);
    }

    constexpr int tile_size = 32;
    const int tiles_x = (W + tile_size - 1) / tile_size;
    const int tiles_y = (H + tile_size - 1) / tile_size;
    const int tile_count = tiles_x * tiles_y;
    std::atomic<bool> cancelled{false};

    std::vector<uint8_t> glitch(static_cast<size_t>(W) * H, 0);
    int total_glitches = 0;

    auto render_pass = [&](const RefOrbit& orbit, double ref_re, double ref_im,
                            const uint8_t* glitch_mask, bool only_glitched) {
        const int orbit_len = static_cast<int>(orbit.z_re.size());
        const double* Or = orbit.z_re.data();
        const double* Oi = orbit.z_im.data();
        int pass_glitches = 0;

        #pragma omp parallel num_threads(thread_count) reduction(+:pass_glitches)
        {
            #pragma omp for schedule(dynamic, 1)
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

                for (int y = y0; y < y1; ++y) {
                    for (int x = x0; x < x1; ++x) {
                        const size_t px_idx = static_cast<size_t>(y) * W + x;
                        if (only_glitched && !glitch_mask[px_idx]) continue;

                        double dx = span_re * ((static_cast<double>(x) + 0.5) / W - 0.5);
                        double dy = -span_im * ((static_cast<double>(y) + 0.5) / H - 0.5);
                        double rot_dx, rot_dy;
                        if (has_rot) {
                            rot_dx = dx * cos_t - dy * sin_t;
                            rot_dy = dx * sin_t + dy * cos_t;
                        } else {
                            rot_dx = dx; rot_dy = dy;
                        }
                        const double px_off_re = rot_dx + (p.center_re - ref_re);
                        const double px_off_im = rot_dy + (p.center_im - ref_im);

                        double dc_re, dc_im, dz_re, dz_im;
                        if (is_julia) {
                            dc_re = 0.0; dc_im = 0.0;
                            dz_re = px_off_re; dz_im = px_off_im;
                        } else {
                            dc_re = px_off_re; dc_im = px_off_im;
                            dz_re = 0.0; dz_im = 0.0;
                        }
                        int iter = max_iter;
                        bool escaped = false;
                        bool is_glitch = false;
                        double escape_mag2 = 0.0;

                        const int limit = std::min(max_iter, orbit_len - 1);
                        for (int n = 0; n < limit; ++n) {
                            const double Zn_re = Or[n];
                            const double Zn_im = Oi[n];
                            const double two_Zn_re = 2.0 * Zn_re;
                            const double two_Zn_im = 2.0 * Zn_im;

                            const double new_dz_re = two_Zn_re * dz_re - two_Zn_im * dz_im
                                                   + dz_re * dz_re - dz_im * dz_im + dc_re;
                            const double new_dz_im = two_Zn_re * dz_im + two_Zn_im * dz_re
                                                   + 2.0 * dz_re * dz_im + dc_im;
                            dz_re = new_dz_re;
                            dz_im = new_dz_im;

                            const double z_re = Or[n + 1] + dz_re;
                            const double z_im = Oi[n + 1] + dz_im;
                            const double mag2 = z_re * z_re + z_im * z_im;

                            if (mag2 > bail2) {
                                iter = n;
                                escaped = true;
                                escape_mag2 = mag2;
                                break;
                            }
                            if (!std::isfinite(dz_re) || !std::isfinite(dz_im)) {
                                is_glitch = true; break;
                            }
                            const double dz_mag2 = dz_re * dz_re + dz_im * dz_im;
                            if (dz_mag2 > 1e-4 && mag2 < GLITCH_TOLERANCE * dz_mag2) {
                                is_glitch = true; break;
                            }
                        }

                        if (is_glitch) {
                            glitch[px_idx] = 1;
                            ++pass_glitches;
                            continue;
                        }
                        if (!escaped && limit < max_iter) {
                            glitch[px_idx] = 2;
                            ++pass_glitches;
                            continue;
                        }
                        glitch[px_idx] = 0;
                        fo.iter_u32[px_idx] = static_cast<uint32_t>(escaped ? iter : max_iter);
                        fo.norm_f32[px_idx] = escaped ? static_cast<float>(escape_mag2) : 0.0f;
                    }
                }
            }
        }
        return pass_glitches;
    };

    total_glitches = render_pass(ref, p.center_re, p.center_im, nullptr, false);

    // Rebase for glitch=1 (cancellation glitches)
    for (int rebase = 0; rebase < 3; ++rebase) {
        if (cancelled.load(std::memory_order_relaxed)) break;
        double rebase_re = p.center_re, rebase_im = p.center_im;
        bool found = false;
        for (int y = 0; y < H && !found; ++y)
            for (int x = 0; x < W && !found; ++x)
                if (glitch[static_cast<size_t>(y) * W + x] == 1) {
                    double dx = span_re * ((static_cast<double>(x) + 0.5) / W - 0.5);
                    double dy = -span_im * ((static_cast<double>(y) + 0.5) / H - 0.5);
                    if (has_rot) {
                        rebase_re = p.center_re + dx * cos_t - dy * sin_t;
                        rebase_im = p.center_im + dx * sin_t + dy * cos_t;
                    } else {
                        rebase_re = p.center_re + dx;
                        rebase_im = p.center_im + dy;
                    }
                    found = true;
                }
        if (!found) break;
        RefOrbit rebase_ref = is_julia
            ? compute_reference_orbit_julia(rebase_re, rebase_im,
                                             p.julia_re, p.julia_im, max_iter, bail2)
            : compute_reference_orbit(rebase_re, rebase_im, max_iter, bail2);
        render_pass(rebase_ref, rebase_re, rebase_im, glitch.data(), true);
    }

    // MPFR fallback for remaining glitch=1 and truncated=2 pixels.
    {
        const int mpfr_bits = std::max(128, static_cast<int>(std::ceil(
            p.scale > 0 ? -std::log2(p.scale) : 53)) + 64);

        #pragma omp parallel num_threads(thread_count)
        {
            #pragma omp for schedule(dynamic, 16)
            for (int idx = 0; idx < W * H; ++idx) {
                if (!glitch[idx]) continue;
                const int x = idx % W;
                const int y = idx / W;
                const double frac_x = (static_cast<double>(x) + 0.5) / W - 0.5;
                const double frac_y = (static_cast<double>(y) + 0.5) / H - 0.5;
                const double dx = span_re * frac_x;
                const double dy = -span_im * frac_y;
                double off_re, off_im;
                if (has_rot) {
                    off_re = dx * cos_t - dy * sin_t;
                    off_im = dx * sin_t + dy * cos_t;
                } else {
                    off_re = dx;
                    off_im = dy;
                }
                int iter = max_iter;
                double escape_mag2 = 0.0;

#if defined(FSD_HAS_MPFR)
                {
                    const mpfr_prec_t prec = static_cast<mpfr_prec_t>(mpfr_bits);
                    MpfrGuard cr(prec), ci(prec), zr(prec), zi(prec);
                    MpfrGuard zr2(prec), zi2(prec), tmp(prec), mag(prec), b2(prec);
                    if (is_julia) {
                        mpfr_set_d(cr, p.julia_re, MPFR_RNDN);
                        mpfr_set_d(ci, p.julia_im, MPFR_RNDN);
                        if (!p.center_re_str.empty())
                            mpfr_set_str(zr, p.center_re_str.c_str(), 10, MPFR_RNDN);
                        else
                            mpfr_set_d(zr, p.center_re, MPFR_RNDN);
                        mpfr_set_d(tmp, off_re, MPFR_RNDN);
                        mpfr_add(zr, zr, tmp, MPFR_RNDN);
                        if (!p.center_im_str.empty())
                            mpfr_set_str(zi, p.center_im_str.c_str(), 10, MPFR_RNDN);
                        else
                            mpfr_set_d(zi, p.center_im, MPFR_RNDN);
                        mpfr_set_d(tmp, off_im, MPFR_RNDN);
                        mpfr_add(zi, zi, tmp, MPFR_RNDN);
                    } else {
                        if (!p.center_re_str.empty())
                            mpfr_set_str(cr, p.center_re_str.c_str(), 10, MPFR_RNDN);
                        else
                            mpfr_set_d(cr, p.center_re, MPFR_RNDN);
                        mpfr_set_d(tmp, off_re, MPFR_RNDN);
                        mpfr_add(cr, cr, tmp, MPFR_RNDN);
                        if (!p.center_im_str.empty())
                            mpfr_set_str(ci, p.center_im_str.c_str(), 10, MPFR_RNDN);
                        else
                            mpfr_set_d(ci, p.center_im, MPFR_RNDN);
                        mpfr_set_d(tmp, off_im, MPFR_RNDN);
                        mpfr_add(ci, ci, tmp, MPFR_RNDN);
                        mpfr_set_d(zr, 0.0, MPFR_RNDN);
                        mpfr_set_d(zi, 0.0, MPFR_RNDN);
                    }
                    mpfr_set_d(b2, bail2, MPFR_RNDN);
                    for (int n = 0; n < max_iter; ++n) {
                        mpfr_mul(zr2, zr, zr, MPFR_RNDN);
                        mpfr_mul(zi2, zi, zi, MPFR_RNDN);
                        mpfr_add(mag, zr2, zi2, MPFR_RNDN);
                        if (mpfr_cmp(mag, b2) > 0) {
                            iter = n;
                            escape_mag2 = mpfr_get_d(mag, MPFR_RNDN);
                            break;
                        }
                        mpfr_sub(tmp, zr2, zi2, MPFR_RNDN);
                        mpfr_add(tmp, tmp, cr, MPFR_RNDN);
                        mpfr_mul(zi, zr, zi, MPFR_RNDN);
                        mpfr_mul_ui(zi, zi, 2, MPFR_RNDN);
                        mpfr_add(zi, zi, ci, MPFR_RNDN);
                        mpfr_set(zr, tmp, MPFR_RNDN);
                    }
                }
#elif defined(FSD_HAS_FLOAT128)
                {
                    __float128 cre, cim, zr, zi;
                    if (is_julia) {
                        cre = static_cast<__float128>(p.julia_re);
                        cim = static_cast<__float128>(p.julia_im);
                        zr = scalar_from_string<__float128>(p.center_re_str, p.center_re)
                            + static_cast<__float128>(off_re);
                        zi = scalar_from_string<__float128>(p.center_im_str, p.center_im)
                            + static_cast<__float128>(off_im);
                    } else {
                        cre = scalar_from_string<__float128>(p.center_re_str, p.center_re)
                            + static_cast<__float128>(off_re);
                        cim = scalar_from_string<__float128>(p.center_im_str, p.center_im)
                            + static_cast<__float128>(off_im);
                        zr = 0; zi = 0;
                    }
                    for (int n = 0; n < max_iter; ++n) {
                        const __float128 zr2 = zr * zr, zi2 = zi * zi;
                        const double m2 = static_cast<double>(zr2 + zi2);
                        if (m2 > bail2) { iter = n; escape_mag2 = m2; break; }
                        const __float128 nzi = static_cast<__float128>(2.0) * zr * zi + cim;
                        zr = zr2 - zi2 + cre;
                        zi = nzi;
                    }
                }
#else
                {
                    double cre, cim, zr, zi;
                    if (is_julia) {
                        cre = p.julia_re; cim = p.julia_im;
                        zr = p.center_re + off_re;
                        zi = p.center_im + off_im;
                    } else {
                        cre = p.center_re + off_re;
                        cim = p.center_im + off_im;
                        zr = 0; zi = 0;
                    }
                    for (int n = 0; n < max_iter; ++n) {
                        const double zr2 = zr * zr, zi2 = zi * zi;
                        if (zr2 + zi2 > bail2) { iter = n; escape_mag2 = zr2 + zi2; break; }
                        const double nzi = 2.0 * zr * zi + cim;
                        zr = zr2 - zi2 + cre;
                        zi = nzi;
                    }
                }
#endif
                fo.iter_u32[idx] = static_cast<uint32_t>(iter);
                fo.norm_f32[idx] = iter < max_iter ? static_cast<float>(escape_mag2) : 0.0f;
            }
        }
    }

    if (cancelled.load(std::memory_order_relaxed)) {
        throw std::runtime_error("cancelled");
    }

    auto t1 = std::chrono::steady_clock::now();
    MapStats stats;
    stats.elapsed_ms  = std::chrono::duration<double, std::milli>(t1 - t0).count();
    stats.pixel_count = W * H;
    stats.scalar_used = "perturbation_fp64";
    stats.engine_used = "openmp";
    fo.scalar_used = stats.scalar_used;
    fo.engine_used = stats.engine_used;
    return stats;
}

} // namespace fsd::compute
