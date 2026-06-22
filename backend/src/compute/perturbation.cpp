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

#ifdef _OPENMP
#  include <omp.h>
#endif

#if defined(FSD_HAS_FLOAT128)
#  include <quadmath.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
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
// Check if perturbation is applicable for this request
// ---------------------------------------------------------------------------

bool perturbation_applicable(const MapParams& p)
{
    if (p.variant != Variant::Mandelbrot) return false;
    if (p.metric  != Metric::Escape)      return false;
    if (p.julia)                           return false;
    if (p.smooth)                          return false;
    if (p.custom_step_fn)                  return false;
    if (p.scale >= 1e-7)                   return false;
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
    const double re_min  = p.center_re - span_re * 0.5;
    const double im_max  = p.center_im + span_im * 0.5;
    const double bail2   = p.bailout_sq;
    const int    max_iter = p.iterations;
    const int    thread_count = resolve_render_threads(p.render_threads);

    // Step 1: compute reference orbit at center
    RefOrbit ref = compute_reference_orbit(p.center_re, p.center_im, max_iter, bail2);

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

                        // Compute delta_c directly from pixel offset to avoid
                        // catastrophic cancellation in (re - ref_re) at deep zoom.
                        const double px_frac_x = (static_cast<double>(x) + 0.5) / W - 0.5;
                        const double px_frac_y = (static_cast<double>(y) + 0.5) / H - 0.5;
                        const double dc_re = span_re * px_frac_x + (p.center_re - ref_re);
                        const double dc_im = -span_im * px_frac_y + (p.center_im - ref_im);

                        double dz_re = 0.0;
                        double dz_im = 0.0;
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
                                iter = n + 1;
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

    // Rebase passes for glitch pixels (up to 3 rebases)
    for (int rebase = 0; rebase < 3 && total_glitches > 0; ++rebase) {
        if (cancelled.load(std::memory_order_relaxed)) break;

        // Find a glitch pixel to use as the new reference
        double rebase_re = p.center_re;
        double rebase_im = p.center_im;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                if (glitch[static_cast<size_t>(y) * W + x]) {
                    rebase_re = re_min + (static_cast<double>(x) + 0.5) / W * span_re;
                    rebase_im = im_max - (static_cast<double>(y) + 0.5) / H * span_im;
                    goto found_rebase;
                }
            }
        }
        found_rebase:

        RefOrbit rebase_ref = compute_reference_orbit(rebase_re, rebase_im, max_iter, bail2);
        total_glitches = render_pass(rebase_ref, rebase_re, rebase_im, glitch.data(), true);
    }

    // Any remaining glitch pixels: fall back to interior (white)
    if (total_glitches > 0) {
        for (int y = 0; y < H; ++y) {
            uint8_t* row = out.ptr<uint8_t>(y);
            for (int x = 0; x < W; ++x) {
                if (glitch[static_cast<size_t>(y) * W + x]) {
                    uint8_t* px = row + 3 * x;
                    px[0] = px[1] = px[2] = 255;
                }
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

} // namespace fsd::compute
