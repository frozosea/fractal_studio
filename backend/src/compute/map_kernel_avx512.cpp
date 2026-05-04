// compute/map_kernel_avx512.cpp
//
// AVX-512F/DQ fractal map renderer — 8 fp64 pixels at a time.
//
// Architecture:
//   zmm regs hold 8 fp64 values. Each "lane" is one pixel. We iterate all 8
//   together until all lanes have escaped or max_iter is reached. Escaped
//   lanes are kept in a bitmask; their zmm values are no longer updated (the
//   mask prevents writing back). This is the "masked iteration" pattern.
//
// Variant support: all 10 quadratic variants are supported in the fp64 path via a
//   runtime switch on variant_id *outside* the pixel loop. Each variant has
//   its own tight inner AVX-512 loop. Fx64 falls through to the OpenMP raw
//   integer path until an IFMA implementation can avoid fp64 escape checks.
//
// Julia mode:
//   When p.julia is true, the pixel coordinate is z0 and the constant c is
//   (p.julia_re, p.julia_im). The Mandelbrot setup is the inverse: z0=0,
//   c=pixel. This is handled by swapping the initialisation of vzre/vzim and
//   vcre/vcim after the coordinate build step.
//
// Non-escape metrics:
//   vmn (min |z|²) and vmx (max |z|²) are tracked across all iterations in
//   the AVX-512 loop. After the loop, per-lane mn/mx are extracted and fed to
//   colorize_field_bgr for MinAbs / MaxAbs / Envelope metrics.
//
#include "map_kernel_avx512.hpp"
#include "colormap.hpp"   // colorize_escape_bgr / colorize_field_bgr
#include "cpu_features.hpp"
#include "parallel.hpp"
#include "variants.hpp"   // Variant enum

#include <opencv2/core.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

// AVX-512 intrinsics are available if __AVX512F__ is defined.
// We compile this file with -mavx512f via a target_compile_options guard in
// CMakeLists.txt so the intrinsics are always available here.
#if defined(__AVX512F__)
#  include <immintrin.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#  include <cpuid.h>  // __get_cpuid_count
#endif

namespace fsd::compute {

bool avx512_available() noexcept {
#if defined(__AVX512F__) && (defined(__x86_64__) || defined(__i386__))
    // Runtime CPUID check — this TU uses AVX-512F plus AVX-512DQ intrinsics.
    if (!avx512_os_state_available()) return false;
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        const bool has_f = static_cast<bool>((ebx >> 16) & 1);
        const bool has_dq = static_cast<bool>((ebx >> 17) & 1);
        return has_f && has_dq;
    }
    return false;
#else
    return false;
#endif
}

bool avx512ifma_available() noexcept {
#if defined(__AVX512F__) && (defined(__x86_64__) || defined(__i386__))
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        return static_cast<bool>((ebx >> 21) & 1);
    }
    return false;
#else
    return false;
#endif
}

#if defined(__AVX512F__)

// ---- fp64 AVX-512 kernel — all 10 variants, Julia mode, metrics 0-3 ----

// Helper: normalize min|z|² or max|z|² to [0,1] for field coloring.
// bail2 = bailout².
static inline double norm2_to_01(double v, double bail2) {
    if (!std::isfinite(v) || v <= 0.0) return 0.0;
    return std::min(1.0, std::sqrt(v) / std::sqrt(bail2));
}

static void avx512_fp64_row(
    int y, int W, int H,
    double re_min, double im_max,
    double span_re, double span_im,
    double bail2, int max_iter,
    int variant_id,
    bool julia, double julia_re, double julia_im,
    Metric metric, Colormap cmap,
    uint8_t* row_ptr
) {
    const double im_d = im_max - (static_cast<double>(y) + 0.5) / H * span_im;
    const __m512d vbail2 = _mm512_set1_pd(bail2);
    const __m512d vtwo   = _mm512_set1_pd(2.0);
    const __m512d vzero  = _mm512_setzero_pd();
    const bool track_min = (metric == Metric::MinAbs || metric == Metric::Envelope);
    const bool track_max = (metric == Metric::MaxAbs || metric == Metric::Envelope);

    // Pixel x stride: 8 lanes at a time.
    for (int x = 0; x < W; x += 8) {
        // Build coordinate values for 8 consecutive pixels.
        double re_arr[8];
        for (int k = 0; k < 8; k++) {
            const int px_x = x + k;
            re_arr[k] = (px_x < W)
                ? re_min + (static_cast<double>(px_x) + 0.5) / W * span_re
                : 1.0e30;  // out-of-bounds lane: escapes immediately
        }
        const __m512d vpx_re = _mm512_loadu_pd(re_arr);
        const __m512d vpx_im = _mm512_set1_pd(im_d);

        // Julia vs Mandelbrot initialisation.
        // Mandelbrot: z0 = 0,   c = pixel coord
        // Julia:      z0 = pixel coord,  c = (julia_re, julia_im)
        __m512d vzre, vzim, vcre, vcim;
        if (julia) {
            vzre = vpx_re;
            vzim = vpx_im;
            vcre = _mm512_set1_pd(julia_re);
            vcim = _mm512_set1_pd(julia_im);
        } else {
            vzre = vzero;
            vzim = vzero;
            vcre = vpx_re;
            vcim = vpx_im;
        }

        __m512i viter  = _mm512_setzero_si512();
        __mmask8 active = 0xFF;

        __m512d vzre2 = _mm512_mul_pd(vzre, vzre);
        __m512d vzim2 = _mm512_mul_pd(vzim, vzim);
        __m512d vmn = _mm512_set1_pd(std::numeric_limits<double>::infinity());
        __m512d vmx = vzero;

        // Select which variant inner loop to run. The switch is *outside* the
        // pixel loop (hoisted per-row) so the branch predictor and the
        // out-of-order engine see a straight-line inner loop with no variant
        // check per iteration.

#define AVX_INNER_LOOP_BEGIN \
        for (int i = 0; i < max_iter && active; i++) {

#define AVX_WRITEBACK_AND_ESCAPE \
            const __m512d new_re2 = _mm512_mul_pd(new_re, new_re); \
            const __m512d new_im2 = _mm512_mul_pd(new_im, new_im); \
            const __m512d vn2 = _mm512_add_pd(new_re2, new_im2); \
            if (track_min) vmn = _mm512_mask_min_pd(vmn, active, vmn, vn2); \
            if (track_max) vmx = _mm512_mask_max_pd(vmx, active, vmx, vn2); \
            const __mmask8 escaped_radius = _mm512_mask_cmp_pd_mask( \
                active, vn2, vbail2, _CMP_GT_OQ); \
            const __mmask8 escaped_nan = _mm512_mask_cmp_pd_mask( \
                active, vn2, vn2, _CMP_UNORD_Q); \
            const __mmask8 escaped = escaped_radius | escaped_nan; \
            vzre = _mm512_mask_mov_pd(vzre, active, new_re); \
            vzim = _mm512_mask_mov_pd(vzim, active, new_im); \
            vzre2 = _mm512_mask_mov_pd(vzre2, active, new_re2); \
            vzim2 = _mm512_mask_mov_pd(vzim2, active, new_im2); \
            if (escaped) { \
                const __m512i vi = _mm512_set1_epi64(i); \
                viter = _mm512_mask_mov_epi64(viter, escaped, vi); \
                active &= ~escaped; \
            } \
        }

        switch (variant_id) {

        // ---- 0: Mandelbrot  z' = z² + c -----------------------------------
        case 0: default:
        AVX_INNER_LOOP_BEGIN
        {
            const __m512d new_re = _mm512_add_pd(_mm512_sub_pd(vzre2, vzim2), vcre);
            const __m512d new_im = _mm512_add_pd(
                _mm512_mul_pd(_mm512_mul_pd(vtwo, vzre), vzim), vcim);
            AVX_WRITEBACK_AND_ESCAPE
        }
        break;

        // ---- 1: Tri (tricorn)  z' = conj(z²) + c --------------------------
        // new_re = re²-im² + cre  (same as Mandelbrot)
        // new_im = -(2*re*im) + cim
        case 1:
        AVX_INNER_LOOP_BEGIN
        {
            const __m512d new_re = _mm512_add_pd(_mm512_sub_pd(vzre2, vzim2), vcre);
            // fnmadd: -(2*re*im) + cim
            const __m512d new_im = _mm512_fnmadd_pd(
                _mm512_mul_pd(vtwo, vzre), vzim, vcim);
            AVX_WRITEBACK_AND_ESCAPE
        }
        break;

        // ---- 2: Boat  w=(|re|,|im|), z'=w²+c ------------------------------
        case 2:
        AVX_INNER_LOOP_BEGIN
        {
            const __m512d wre    = _mm512_abs_pd(vzre);
            const __m512d wim    = _mm512_abs_pd(vzim);
            const __m512d new_re = _mm512_add_pd(_mm512_sub_pd(vzre2, vzim2), vcre);
            const __m512d new_im = _mm512_add_pd(
                _mm512_mul_pd(_mm512_mul_pd(vtwo, wre), wim), vcim);
            AVX_WRITEBACK_AND_ESCAPE
        }
        break;

        // ---- 3: Duck  w=(re,|im|), z'=w²+c --------------------------------
        case 3:
        AVX_INNER_LOOP_BEGIN
        {
            const __m512d wim    = _mm512_abs_pd(vzim);
            const __m512d new_re = _mm512_add_pd(_mm512_sub_pd(vzre2, vzim2), vcre);
            const __m512d new_im = _mm512_add_pd(
                _mm512_mul_pd(_mm512_mul_pd(vtwo, vzre), wim), vcim);
            AVX_WRITEBACK_AND_ESCAPE
        }
        break;

        // ---- 4: Bell  w=(|re|,-im), z'=w²+c --------------------------------
        case 4:
        AVX_INNER_LOOP_BEGIN
        {
            const __m512d wre    = _mm512_abs_pd(vzre);
            const __m512d wim    = _mm512_sub_pd(vzero, vzim);  // -vzim
            const __m512d new_re = _mm512_add_pd(_mm512_sub_pd(vzre2, vzim2), vcre);
            const __m512d new_im = _mm512_add_pd(
                _mm512_mul_pd(_mm512_mul_pd(vtwo, wre), wim), vcim);
            AVX_WRITEBACK_AND_ESCAPE
        }
        break;

        // ---- 5: Fish  sq=z²; new=(|sq.re|, sq.im)+c -----------------------
        case 5:
        AVX_INNER_LOOP_BEGIN
        {
            const __m512d sq_re  = _mm512_sub_pd(vzre2, vzim2);
            const __m512d sq_im  = _mm512_mul_pd(_mm512_mul_pd(vtwo, vzre), vzim);
            const __m512d new_re = _mm512_add_pd(_mm512_abs_pd(sq_re), vcre);
            const __m512d new_im = _mm512_add_pd(sq_im, vcim);
            AVX_WRITEBACK_AND_ESCAPE
        }
        break;

        // ---- 6: Vase  sq=z²; new=(|sq.re|, -sq.im)+c ----------------------
        case 6:
        AVX_INNER_LOOP_BEGIN
        {
            const __m512d sq_re  = _mm512_sub_pd(vzre2, vzim2);
            const __m512d sq_im  = _mm512_mul_pd(_mm512_mul_pd(vtwo, vzre), vzim);
            const __m512d new_re = _mm512_add_pd(_mm512_abs_pd(sq_re), vcre);
            // fnmadd: -(sq_im) + vcim
            const __m512d new_im = _mm512_fnmadd_pd(
                _mm512_set1_pd(1.0), sq_im, vcim);
            AVX_WRITEBACK_AND_ESCAPE
        }
        break;

        // ---- 7: Bird  sq=z²; new=(|sq.re|,|sq.im|)+c ----------------------
        case 7:
        AVX_INNER_LOOP_BEGIN
        {
            const __m512d sq_re  = _mm512_sub_pd(vzre2, vzim2);
            const __m512d sq_im  = _mm512_mul_pd(_mm512_mul_pd(vtwo, vzre), vzim);
            const __m512d new_re = _mm512_add_pd(_mm512_abs_pd(sq_re), vcre);
            const __m512d new_im = _mm512_add_pd(_mm512_abs_pd(sq_im), vcim);
            AVX_WRITEBACK_AND_ESCAPE
        }
        break;

        // ---- 8: Mask  w=(re,|im|), sq=w²; new=(|sq.re|,sq.im)+c -----------
        case 8:
        AVX_INNER_LOOP_BEGIN
        {
            const __m512d wim    = _mm512_abs_pd(vzim);
            const __m512d sq_re  = _mm512_sub_pd(vzre2, vzim2);
            const __m512d sq_im  = _mm512_mul_pd(_mm512_mul_pd(vtwo, vzre), wim);
            const __m512d new_re = _mm512_add_pd(_mm512_abs_pd(sq_re), vcre);
            const __m512d new_im = _mm512_add_pd(sq_im, vcim);
            AVX_WRITEBACK_AND_ESCAPE
        }
        break;

        // ---- 9: Ship  w=(|re|,im), sq=w²; new=(|sq.re|,-sq.im)+c ----------
        case 9:
        AVX_INNER_LOOP_BEGIN
        {
            const __m512d wre    = _mm512_abs_pd(vzre);
            const __m512d sq_re  = _mm512_sub_pd(vzre2, vzim2);
            const __m512d sq_im  = _mm512_mul_pd(_mm512_mul_pd(vtwo, wre), vzim);
            const __m512d new_re = _mm512_add_pd(_mm512_abs_pd(sq_re), vcre);
            // fnmadd: -(sq_im) + vcim
            const __m512d new_im = _mm512_fnmadd_pd(
                _mm512_set1_pd(1.0), sq_im, vcim);
            AVX_WRITEBACK_AND_ESCAPE
        }
        break;

        } // switch (variant_id)

#undef AVX_INNER_LOOP_BEGIN
#undef AVX_WRITEBACK_AND_ESCAPE

        // Write output pixels (up to 8, clamped to actual W).
        int64_t iters_arr[8];
        _mm512_storeu_si512(iters_arr, viter);
        double mn_arr[8], mx_arr[8];
        if (track_min) _mm512_storeu_pd(mn_arr, vmn);
        if (track_max) _mm512_storeu_pd(mx_arr, vmx);

        for (int k = 0; k < 8 && (x + k) < W; k++) {
            uint8_t* px = row_ptr + 3 * (x + k);
            const bool escaped_k = !((active >> k) & 1);

            if (metric == Metric::Escape) {
                const int it = escaped_k ? static_cast<int>(iters_arr[k]) : max_iter;
                // norm not tracked in AVX-512 path; smooth mode excluded at dispatch.
                colorize_escape_bgr(it, max_iter, cmap, 0.0, false, px[0], px[1], px[2]);
            } else if (metric == Metric::MinAbs) {
                const double v01 = norm2_to_01(mn_arr[k], bail2);
                colorize_field_bgr(v01, cmap, px[0], px[1], px[2]);
            } else if (metric == Metric::MaxAbs) {
                const double v01 = norm2_to_01(mx_arr[k], bail2);
                colorize_field_bgr(v01, cmap, px[0], px[1], px[2]);
            } else {
                // Envelope: combine min+max.
                const double mn_v = norm2_to_01(mn_arr[k], bail2);
                const double mx_v = norm2_to_01(mx_arr[k], bail2);
                colorize_field_bgr(0.5 * (mn_v + mx_v), cmap, px[0], px[1], px[2]);
            }
        }
    }
}

MapStats render_map_avx512_fp64(const MapParams& p, cv::Mat& out) {
    if (out.empty() || out.rows != p.height || out.cols != p.width || out.type() != CV_8UC3) {
        out.create(p.height, p.width, CV_8UC3);
    }

    const int W = p.width, H = p.height;
    const double aspect  = static_cast<double>(W) / H;
    const double span_im = p.scale;
    const double span_re = p.scale * aspect;
    const double re_min  = p.center_re - span_re * 0.5;
    const double im_max  = p.center_im + span_im * 0.5;
    const double bail2   = p.bailout_sq;
    const int variant_id = static_cast<int>(p.variant);
    const int thread_count = resolve_render_threads(p.render_threads);
    std::atomic<bool> cancelled{false};

    const auto t0 = std::chrono::steady_clock::now();

    #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 4)
    for (int y = 0; y < H; y++) {
        if (cancelled.load(std::memory_order_relaxed)) continue;
        if (map_render_cancel_requested(p)) {
            cancelled.store(true, std::memory_order_relaxed);
            continue;
        }
        avx512_fp64_row(
            y, W, H, re_min, im_max, span_re, span_im,
            bail2, p.iterations,
            variant_id,
            p.julia, p.julia_re, p.julia_im,
            p.metric, p.colormap,
            out.ptr<uint8_t>(y)
        );
    }
    if (cancelled.load(std::memory_order_relaxed) || map_render_cancel_requested(p)) {
        throw std::runtime_error("cancelled");
    }

    const auto t1 = std::chrono::steady_clock::now();
    MapStats s;
    s.elapsed_ms  = std::chrono::duration<double, std::milli>(t1 - t0).count();
    s.pixel_count = W * H;
    s.scalar_used = "fp64";
    s.engine_used = "avx512";
    return s;
}

MapStats render_map_avx512_fx64(const MapParams& p, cv::Mat& out) {
    (void)p; (void)out;
    MapStats s;
    s.scalar_used = "fx64";
    s.engine_used = "openmp_fallback";
    return s;
}

#else  // !__AVX512F__

// Stub implementations when AVX-512 is not available at compile time.
MapStats render_map_avx512_fp64(const MapParams& p, cv::Mat& out) {
    (void)p; (void)out;
    MapStats s; s.engine_used = "openmp_fallback"; return s;
}
MapStats render_map_avx512_fx64(const MapParams& p, cv::Mat& out) {
    (void)p; (void)out;
    MapStats s; s.engine_used = "openmp_fallback"; return s;
}

#endif  // __AVX512F__

} // namespace fsd::compute
