// compute/map_kernel_avx2.cpp

#include "map_kernel_avx2.hpp"

#include "colormap.hpp"
#include "parallel.hpp"

#include <opencv2/core.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

#if defined(__AVX2__)
#  include <immintrin.h>
#endif

namespace fsd::compute {

#if defined(__AVX2__) && defined(__FMA__)

namespace {

inline __m256d avx2_abs_pd(__m256d v) {
    const __m256d sign = _mm256_set1_pd(-0.0);
    return _mm256_andnot_pd(sign, v);
}

inline __m256d avx2_lane_mask_pd(int mask) {
    const __m256i bits = _mm256_set_epi64x(
        (mask & 8) ? -1LL : 0LL,
        (mask & 4) ? -1LL : 0LL,
        (mask & 2) ? -1LL : 0LL,
        (mask & 1) ? -1LL : 0LL);
    return _mm256_castsi256_pd(bits);
}

inline double norm2_to_01(double v, double bail2) {
    if (!std::isfinite(v) || v <= 0.0) return 0.0;
    return std::min(1.0, std::sqrt(v) / std::sqrt(bail2));
}

template <int VariantId>
inline void avx2_step(
    __m256d zre,
    __m256d zim,
    __m256d zre2,
    __m256d zim2,
    __m256d cre,
    __m256d cim,
    __m256d& new_re,
    __m256d& new_im
) {
    const __m256d two = _mm256_set1_pd(2.0);
    const __m256d zero = _mm256_setzero_pd();
    const __m256d sq_re = _mm256_sub_pd(zre2, zim2);
    if constexpr (VariantId == 1) {
        const __m256d sq_im = _mm256_mul_pd(_mm256_mul_pd(two, zre), zim);
        new_re = _mm256_add_pd(sq_re, cre);
        new_im = _mm256_fnmadd_pd(_mm256_set1_pd(1.0), sq_im, cim);
    } else if constexpr (VariantId == 2) {
        const __m256d wre = avx2_abs_pd(zre);
        const __m256d wim = avx2_abs_pd(zim);
        new_re = _mm256_add_pd(sq_re, cre);
        new_im = _mm256_add_pd(_mm256_mul_pd(_mm256_mul_pd(two, wre), wim), cim);
    } else if constexpr (VariantId == 3) {
        const __m256d wim = avx2_abs_pd(zim);
        new_re = _mm256_add_pd(sq_re, cre);
        new_im = _mm256_add_pd(_mm256_mul_pd(_mm256_mul_pd(two, zre), wim), cim);
    } else if constexpr (VariantId == 4) {
        const __m256d wre = avx2_abs_pd(zre);
        const __m256d wim = _mm256_sub_pd(zero, zim);
        new_re = _mm256_add_pd(sq_re, cre);
        new_im = _mm256_add_pd(_mm256_mul_pd(_mm256_mul_pd(two, wre), wim), cim);
    } else if constexpr (VariantId == 5) {
        const __m256d sq_im = _mm256_mul_pd(_mm256_mul_pd(two, zre), zim);
        new_re = _mm256_add_pd(avx2_abs_pd(sq_re), cre);
        new_im = _mm256_add_pd(sq_im, cim);
    } else if constexpr (VariantId == 6) {
        const __m256d sq_im = _mm256_mul_pd(_mm256_mul_pd(two, zre), zim);
        new_re = _mm256_add_pd(avx2_abs_pd(sq_re), cre);
        new_im = _mm256_fnmadd_pd(_mm256_set1_pd(1.0), sq_im, cim);
    } else if constexpr (VariantId == 7) {
        const __m256d sq_im = _mm256_mul_pd(_mm256_mul_pd(two, zre), zim);
        new_re = _mm256_add_pd(avx2_abs_pd(sq_re), cre);
        new_im = _mm256_add_pd(avx2_abs_pd(sq_im), cim);
    } else if constexpr (VariantId == 8) {
        const __m256d wim = avx2_abs_pd(zim);
        const __m256d sq_im = _mm256_mul_pd(_mm256_mul_pd(two, zre), wim);
        new_re = _mm256_add_pd(avx2_abs_pd(sq_re), cre);
        new_im = _mm256_add_pd(sq_im, cim);
    } else if constexpr (VariantId == 9) {
        const __m256d wre = avx2_abs_pd(zre);
        const __m256d sq_im = _mm256_mul_pd(_mm256_mul_pd(two, wre), zim);
        new_re = _mm256_add_pd(avx2_abs_pd(sq_re), cre);
        new_im = _mm256_fnmadd_pd(_mm256_set1_pd(1.0), sq_im, cim);
    } else {
        new_re = _mm256_add_pd(sq_re, cre);
        new_im = _mm256_add_pd(_mm256_mul_pd(_mm256_mul_pd(two, zre), zim), cim);
    }
}

template <int VariantId>
void avx2_fp64_row(
    int y, int W, int H,
    double re_min, double im_max,
    double span_re, double span_im,
    double bail2, int max_iter,
    bool julia, double julia_re, double julia_im,
    Metric metric, Colormap cmap,
    uint8_t* row_ptr
) {
    const double im_d = im_max - (static_cast<double>(y) + 0.5) / H * span_im;
    const __m256d vbail2 = _mm256_set1_pd(bail2);
    const __m256d vzero = _mm256_setzero_pd();
    const bool track_min = (metric == Metric::MinAbs || metric == Metric::Envelope);
    const bool track_max = (metric == Metric::MaxAbs || metric == Metric::Envelope);

    for (int x = 0; x < W; x += 4) {
        double re_arr[4];
        int iter_arr[4] = {max_iter, max_iter, max_iter, max_iter};
        int lane_mask = 0;
        for (int k = 0; k < 4; ++k) {
            const int px_x = x + k;
            if (px_x < W) {
                re_arr[k] = re_min + (static_cast<double>(px_x) + 0.5) / W * span_re;
                lane_mask |= (1 << k);
            } else {
                re_arr[k] = 1.0e30;
            }
        }

        const __m256d vpx_re = _mm256_loadu_pd(re_arr);
        const __m256d vpx_im = _mm256_set1_pd(im_d);

        __m256d zre, zim, cre, cim;
        if (julia) {
            zre = vpx_re;
            zim = vpx_im;
            cre = _mm256_set1_pd(julia_re);
            cim = _mm256_set1_pd(julia_im);
        } else {
            zre = vzero;
            zim = vzero;
            cre = vpx_re;
            cim = vpx_im;
        }

        __m256d zre2 = _mm256_mul_pd(zre, zre);
        __m256d zim2 = _mm256_mul_pd(zim, zim);
        __m256d vmn = _mm256_set1_pd(std::numeric_limits<double>::infinity());
        __m256d vmx = vzero;

        int active = lane_mask;
        for (int i = 0; i < max_iter && active; ++i) {
            const __m256d active_vec = avx2_lane_mask_pd(active);
            __m256d new_re, new_im;
            avx2_step<VariantId>(zre, zim, zre2, zim2, cre, cim, new_re, new_im);

            const __m256d new_re2 = _mm256_mul_pd(new_re, new_re);
            const __m256d new_im2 = _mm256_mul_pd(new_im, new_im);
            const __m256d n2 = _mm256_add_pd(new_re2, new_im2);
            if (track_min) {
                vmn = _mm256_blendv_pd(vmn, _mm256_min_pd(vmn, n2), active_vec);
            }
            if (track_max) {
                vmx = _mm256_blendv_pd(vmx, _mm256_max_pd(vmx, n2), active_vec);
            }

            const __m256d escaped_radius = _mm256_cmp_pd(n2, vbail2, _CMP_GT_OQ);
            const __m256d escaped_nan = _mm256_cmp_pd(n2, n2, _CMP_UNORD_Q);
            const int escaped = _mm256_movemask_pd(_mm256_or_pd(escaped_radius, escaped_nan)) & active;
            if (escaped) {
                for (int k = 0; k < 4; ++k) {
                    if (escaped & (1 << k)) iter_arr[k] = i;
                }
                active &= ~escaped;
            }

            zre = _mm256_blendv_pd(zre, new_re, active_vec);
            zim = _mm256_blendv_pd(zim, new_im, active_vec);
            zre2 = _mm256_blendv_pd(zre2, new_re2, active_vec);
            zim2 = _mm256_blendv_pd(zim2, new_im2, active_vec);
        }

        double mn_arr[4], mx_arr[4];
        if (track_min) _mm256_storeu_pd(mn_arr, vmn);
        if (track_max) _mm256_storeu_pd(mx_arr, vmx);

        for (int k = 0; k < 4 && (x + k) < W; ++k) {
            uint8_t* px = row_ptr + 3 * (x + k);
            const bool escaped_k = !((active >> k) & 1);
            if (metric == Metric::Escape) {
                const int it = escaped_k ? iter_arr[k] : max_iter;
                colorize_escape_bgr(it, max_iter, cmap, 0.0, false, px[0], px[1], px[2]);
            } else if (metric == Metric::MinAbs) {
                colorize_field_bgr(norm2_to_01(mn_arr[k], bail2), cmap, px[0], px[1], px[2]);
            } else if (metric == Metric::MaxAbs) {
                colorize_field_bgr(norm2_to_01(mx_arr[k], bail2), cmap, px[0], px[1], px[2]);
            } else {
                const double mn_v = norm2_to_01(mn_arr[k], bail2);
                const double mx_v = norm2_to_01(mx_arr[k], bail2);
                colorize_field_bgr(0.5 * (mn_v + mx_v), cmap, px[0], px[1], px[2]);
            }
        }
    }
}

template <int VariantId>
MapStats render_avx2_variant(const MapParams& p, cv::Mat& out) {
    if (out.empty() || out.rows != p.height || out.cols != p.width || out.type() != CV_8UC3) {
        out.create(p.height, p.width, CV_8UC3);
    }

    const int W = p.width;
    const int H = p.height;
    const double aspect = static_cast<double>(W) / H;
    const double span_im = p.scale;
    const double span_re = p.scale * aspect;
    const double re_min = p.center_re - span_re * 0.5;
    const double im_max = p.center_im + span_im * 0.5;
    const int thread_count = resolve_render_threads(p.render_threads);
    std::atomic<bool> cancelled{false};

    const auto t0 = std::chrono::steady_clock::now();
    #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 4)
    for (int y = 0; y < H; ++y) {
        if (cancelled.load(std::memory_order_relaxed)) continue;
        if (map_render_cancel_requested(p)) {
            cancelled.store(true, std::memory_order_relaxed);
            continue;
        }
        avx2_fp64_row<VariantId>(
            y, W, H, re_min, im_max, span_re, span_im,
            p.bailout_sq, p.iterations,
            p.julia, p.julia_re, p.julia_im,
            p.metric, p.colormap, out.ptr<uint8_t>(y));
    }
    if (cancelled.load(std::memory_order_relaxed) || map_render_cancel_requested(p)) {
        throw std::runtime_error("cancelled");
    }

    const auto t1 = std::chrono::steady_clock::now();
    MapStats s;
    s.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    s.pixel_count = W * H;
    s.scalar_used = "fp64";
    s.engine_used = "avx2";
    return s;
}

} // namespace

MapStats render_map_avx2_fp64(const MapParams& p, cv::Mat& out) {
    switch (static_cast<int>(p.variant)) {
        case 1: return render_avx2_variant<1>(p, out);
        case 2: return render_avx2_variant<2>(p, out);
        case 3: return render_avx2_variant<3>(p, out);
        case 4: return render_avx2_variant<4>(p, out);
        case 5: return render_avx2_variant<5>(p, out);
        case 6: return render_avx2_variant<6>(p, out);
        case 7: return render_avx2_variant<7>(p, out);
        case 8: return render_avx2_variant<8>(p, out);
        case 9: return render_avx2_variant<9>(p, out);
        default: return render_avx2_variant<0>(p, out);
    }
}

#else

MapStats render_map_avx2_fp64(const MapParams& p, cv::Mat& out) {
    (void)p; (void)out;
    MapStats s;
    s.scalar_used = "fp64";
    s.engine_used = "openmp_fallback";
    return s;
}

#endif

} // namespace fsd::compute
