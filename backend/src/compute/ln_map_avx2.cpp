// compute/ln_map_avx2.cpp

#include "ln_map.hpp"

#include "map_kernel_avx2.hpp"
#include "parallel.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#if defined(__AVX2__) && defined(__FMA__)
#  include <immintrin.h>
#endif

namespace fsd::compute {

#if defined(__AVX2__) && defined(__FMA__)
namespace {

constexpr double TAU = 6.283185307179586;
constexpr double LN_FOUR = 1.3862943611198906;

inline __m256d abs_pd(__m256d v) {
    const __m256d sign = _mm256_set1_pd(-0.0);
    return _mm256_andnot_pd(sign, v);
}

inline __m256 abs_ps(__m256 v) {
    const __m256 sign = _mm256_set1_ps(-0.0f);
    return _mm256_andnot_ps(sign, v);
}

inline __m256d lane_mask_pd(int mask) {
    const __m256i bits = _mm256_set_epi64x(
        (mask & 8) ? -1LL : 0LL,
        (mask & 4) ? -1LL : 0LL,
        (mask & 2) ? -1LL : 0LL,
        (mask & 1) ? -1LL : 0LL);
    return _mm256_castsi256_pd(bits);
}

inline __m256 lane_mask_ps(int mask) {
    const __m256i bits = _mm256_set_epi32(
        (mask & 128) ? -1 : 0,
        (mask & 64) ? -1 : 0,
        (mask & 32) ? -1 : 0,
        (mask & 16) ? -1 : 0,
        (mask & 8) ? -1 : 0,
        (mask & 4) ? -1 : 0,
        (mask & 2) ? -1 : 0,
        (mask & 1) ? -1 : 0);
    return _mm256_castsi256_ps(bits);
}

inline int lane_mask(int base, int count) {
    int mask = 0;
    for (int k = 0; k < 4; ++k) {
        if (base + k < count) mask |= (1 << k);
    }
    return mask;
}

inline int lane_mask8(int base, int count) {
    int mask = 0;
    for (int k = 0; k < 8; ++k) {
        if (base + k < count) mask |= (1 << k);
    }
    return mask;
}

inline void step_variant(
    int variant,
    __m256d zre,
    __m256d zim,
    __m256d zre2,
    __m256d zim2,
    __m256d cre,
    __m256d cim,
    __m256d& nre,
    __m256d& nim
) {
    const __m256d two = _mm256_set1_pd(2.0);
    const __m256d zero = _mm256_setzero_pd();
    const __m256d sq_re = _mm256_sub_pd(zre2, zim2);
    const __m256d sq_im = _mm256_mul_pd(_mm256_mul_pd(two, zre), zim);

    switch (variant) {
        case 1:
            nre = _mm256_add_pd(sq_re, cre);
            nim = _mm256_sub_pd(cim, sq_im);
            break;
        case 2:
            nre = _mm256_add_pd(sq_re, cre);
            nim = _mm256_add_pd(_mm256_mul_pd(_mm256_mul_pd(two, abs_pd(zre)), abs_pd(zim)), cim);
            break;
        case 3:
            nre = _mm256_add_pd(sq_re, cre);
            nim = _mm256_add_pd(_mm256_mul_pd(_mm256_mul_pd(two, zre), abs_pd(zim)), cim);
            break;
        case 4:
            nre = _mm256_add_pd(sq_re, cre);
            nim = _mm256_add_pd(_mm256_mul_pd(_mm256_mul_pd(two, abs_pd(zre)), _mm256_sub_pd(zero, zim)), cim);
            break;
        case 5:
            nre = _mm256_add_pd(abs_pd(sq_re), cre);
            nim = _mm256_add_pd(sq_im, cim);
            break;
        case 6:
            nre = _mm256_add_pd(abs_pd(sq_re), cre);
            nim = _mm256_sub_pd(cim, sq_im);
            break;
        case 7:
            nre = _mm256_add_pd(abs_pd(sq_re), cre);
            nim = _mm256_add_pd(abs_pd(sq_im), cim);
            break;
        case 8:
            nre = _mm256_add_pd(abs_pd(sq_re), cre);
            nim = _mm256_add_pd(_mm256_mul_pd(_mm256_mul_pd(two, zre), abs_pd(zim)), cim);
            break;
        case 9:
            nre = _mm256_add_pd(abs_pd(sq_re), cre);
            nim = _mm256_sub_pd(cim, _mm256_mul_pd(_mm256_mul_pd(two, abs_pd(zre)), zim));
            break;
        default:
            nre = _mm256_add_pd(sq_re, cre);
            nim = _mm256_add_pd(sq_im, cim);
            break;
    }
}

inline void step_variant_ps(
    int variant,
    __m256 zre,
    __m256 zim,
    __m256 zre2,
    __m256 zim2,
    __m256 cre,
    __m256 cim,
    __m256& nre,
    __m256& nim
) {
    const __m256 two = _mm256_set1_ps(2.0f);
    const __m256 zero = _mm256_setzero_ps();
    const __m256 sq_re = _mm256_sub_ps(zre2, zim2);
    const __m256 sq_im = _mm256_mul_ps(_mm256_mul_ps(two, zre), zim);

    switch (variant) {
        case 1:
            nre = _mm256_add_ps(sq_re, cre);
            nim = _mm256_sub_ps(cim, sq_im);
            break;
        case 2:
            nre = _mm256_add_ps(sq_re, cre);
            nim = _mm256_add_ps(_mm256_mul_ps(_mm256_mul_ps(two, abs_ps(zre)), abs_ps(zim)), cim);
            break;
        case 3:
            nre = _mm256_add_ps(sq_re, cre);
            nim = _mm256_add_ps(_mm256_mul_ps(_mm256_mul_ps(two, zre), abs_ps(zim)), cim);
            break;
        case 4:
            nre = _mm256_add_ps(sq_re, cre);
            nim = _mm256_add_ps(_mm256_mul_ps(_mm256_mul_ps(two, abs_ps(zre)), _mm256_sub_ps(zero, zim)), cim);
            break;
        case 5:
            nre = _mm256_add_ps(abs_ps(sq_re), cre);
            nim = _mm256_add_ps(sq_im, cim);
            break;
        case 6:
            nre = _mm256_add_ps(abs_ps(sq_re), cre);
            nim = _mm256_sub_ps(cim, sq_im);
            break;
        case 7:
            nre = _mm256_add_ps(abs_ps(sq_re), cre);
            nim = _mm256_add_ps(abs_ps(sq_im), cim);
            break;
        case 8:
            nre = _mm256_add_ps(abs_ps(sq_re), cre);
            nim = _mm256_add_ps(_mm256_mul_ps(_mm256_mul_ps(two, zre), abs_ps(zim)), cim);
            break;
        case 9:
            nre = _mm256_add_ps(abs_ps(sq_re), cre);
            nim = _mm256_sub_ps(cim, _mm256_mul_ps(_mm256_mul_ps(two, abs_ps(zre)), zim));
            break;
        default:
            nre = _mm256_add_ps(sq_re, cre);
            nim = _mm256_add_ps(sq_im, cim);
            break;
    }
}

void ensure_out(const LnMapParams& p, cv::Mat& out) {
    if (out.empty() || out.rows != p.height_t || out.cols != p.width_s || out.type() != CV_8UC3) {
        out.create(p.height_t, p.width_s, CV_8UC3);
    }
}

std::pair<int, int> clamp_rows(const LnMapParams& p, int row_start, int row_count) {
    const int start = std::max(0, std::min(row_start, p.height_t));
    const int end = std::max(start, std::min(p.height_t, start + std::max(0, row_count)));
    return {start, end};
}

struct TrigColumns {
    std::vector<double> cos_col;
    std::vector<double> sin_col;
};

struct TrigColumnsF {
    std::vector<float> cos_col;
    std::vector<float> sin_col;
};

TrigColumns make_trig_columns(int s) {
    TrigColumns cols;
    cols.cos_col.resize(static_cast<size_t>(s));
    cols.sin_col.resize(static_cast<size_t>(s));
    for (int x = 0; x < s; x++) {
        const double th = TAU * static_cast<double>(x) / static_cast<double>(s);
        cols.cos_col[static_cast<size_t>(x)] = std::cos(th);
        cols.sin_col[static_cast<size_t>(x)] = std::sin(th);
    }
    return cols;
}

TrigColumnsF make_trig_columns_f(int s) {
    constexpr float tau = static_cast<float>(TAU);
    TrigColumnsF cols;
    cols.cos_col.resize(static_cast<size_t>(s));
    cols.sin_col.resize(static_cast<size_t>(s));
    for (int x = 0; x < s; x++) {
        const float th = tau * static_cast<float>(x) / static_cast<float>(s);
        cols.cos_col[static_cast<size_t>(x)] = std::cos(th);
        cols.sin_col[static_cast<size_t>(x)] = std::sin(th);
    }
    return cols;
}

void render_rows_impl(
    const LnMapParams& p,
    cv::Mat& out,
    int row_start,
    int row_end,
    bool threaded,
    const LnMapProgress& on_row_done
) {
    const int s = p.width_s;
    const TrigColumns cols = make_trig_columns(s);
    const int variant = static_cast<int>(p.variant);
    const __m256d vbail2 = _mm256_set1_pd(p.bailout_sq);
    const __m256d vzero = _mm256_setzero_pd();
    const __m256d vjre = _mm256_set1_pd(p.julia_re);
    const __m256d vjim = _mm256_set1_pd(p.julia_im);
    std::atomic<int> rows_done{0};

    auto render_row = [&](int row) {
        uint8_t* rowp = out.ptr<uint8_t>(row);
        const double k = LN_FOUR - static_cast<double>(row) * TAU / static_cast<double>(s);
        const double r_mag = std::exp(k);

        for (int col = 0; col < s; col += 4) {
            alignas(32) double pre_arr[4] = {};
            alignas(32) double pim_arr[4] = {};
            for (int lane = 0; lane < 4 && col + lane < s; ++lane) {
                const size_t idx = static_cast<size_t>(col + lane);
                pre_arr[lane] = p.center_re + r_mag * cols.cos_col[idx];
                pim_arr[lane] = p.center_im + r_mag * cols.sin_col[idx];
            }

            const int valid = lane_mask(col, s);
            const __m256d valid_vec = lane_mask_pd(valid);
            const __m256d vpre = _mm256_load_pd(pre_arr);
            const __m256d vpim = _mm256_load_pd(pim_arr);
            __m256d zre = p.julia ? vpre : vzero;
            __m256d zim = p.julia ? vpim : vzero;
            const __m256d cre = p.julia ? vjre : vpre;
            const __m256d cim = p.julia ? vjim : vpim;
            __m256d zre2 = _mm256_mul_pd(zre, zre);
            __m256d zim2 = _mm256_mul_pd(zim, zim);

            int iter_arr[4] = {p.iterations, p.iterations, p.iterations, p.iterations};
            int active = valid;
            for (int iter = 0; iter < p.iterations && active; ++iter) {
                __m256d nre, nim;
                step_variant(variant, zre, zim, zre2, zim2, cre, cim, nre, nim);
                const __m256d nre2 = _mm256_mul_pd(nre, nre);
                const __m256d nim2 = _mm256_mul_pd(nim, nim);
                const __m256d norm2 = _mm256_add_pd(nre2, nim2);
                const __m256d escaped_vec = _mm256_and_pd(
                    valid_vec,
                    _mm256_or_pd(
                        _mm256_cmp_pd(norm2, vbail2, _CMP_GT_OQ),
                        _mm256_cmp_pd(norm2, norm2, _CMP_UNORD_Q)));
                const int escaped = _mm256_movemask_pd(escaped_vec) & active;
                if (escaped) {
                    for (int lane = 0; lane < 4; ++lane) {
                        if (escaped & (1 << lane)) iter_arr[lane] = iter;
                    }
                    active &= ~escaped;
                }
                const __m256d active_vec = lane_mask_pd(active);
                zre = _mm256_blendv_pd(zre, nre, active_vec);
                zim = _mm256_blendv_pd(zim, nim, active_vec);
                zre2 = _mm256_blendv_pd(zre2, nre2, active_vec);
                zim2 = _mm256_blendv_pd(zim2, nim2, active_vec);
            }

            for (int lane = 0; lane < 4 && col + lane < s; ++lane) {
                uint8_t* px = rowp + 3 * (col + lane);
                colorize_escape_bgr(iter_arr[lane], p.iterations, p.colormap, 0.0, false, px[0], px[1], px[2]);
            }
        }

        if (on_row_done) {
            const int done = rows_done.fetch_add(1, std::memory_order_relaxed) + 1;
            if (done == row_end - row_start || (done % 16) == 0) on_row_done(done);
        }
    };

    if (threaded) {
        const int thread_count = default_render_threads();
        #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 4)
        for (int row = row_start; row < row_end; ++row) {
            render_row(row);
        }
    } else {
        for (int row = row_start; row < row_end; ++row) {
            render_row(row);
        }
    }
}

void render_rows_fp32_impl(
    const LnMapParams& p,
    cv::Mat& out,
    int row_start,
    int row_end,
    bool threaded,
    const LnMapProgress& on_row_done
) {
    const int s = p.width_s;
    const TrigColumnsF cols = make_trig_columns_f(s);
    const int variant = static_cast<int>(p.variant);
    const float center_re = static_cast<float>(p.center_re);
    const float center_im = static_cast<float>(p.center_im);
    const __m256 vbail2 = _mm256_set1_ps(static_cast<float>(p.bailout_sq));
    const __m256 vzero = _mm256_setzero_ps();
    const __m256 vjre = _mm256_set1_ps(static_cast<float>(p.julia_re));
    const __m256 vjim = _mm256_set1_ps(static_cast<float>(p.julia_im));
    constexpr float tau = static_cast<float>(TAU);
    constexpr float ln_four = static_cast<float>(LN_FOUR);
    std::atomic<int> rows_done{0};

    auto render_row = [&](int row) {
        uint8_t* rowp = out.ptr<uint8_t>(row);
        const float k = ln_four - static_cast<float>(row) * tau / static_cast<float>(s);
        const float r_mag = std::exp(k);

        for (int col = 0; col < s; col += 8) {
            alignas(32) float pre_arr[8] = {};
            alignas(32) float pim_arr[8] = {};
            for (int lane = 0; lane < 8 && col + lane < s; ++lane) {
                const size_t idx = static_cast<size_t>(col + lane);
                pre_arr[lane] = center_re + r_mag * cols.cos_col[idx];
                pim_arr[lane] = center_im + r_mag * cols.sin_col[idx];
            }

            const int valid = lane_mask8(col, s);
            const __m256 valid_vec = lane_mask_ps(valid);
            const __m256 vpre = _mm256_load_ps(pre_arr);
            const __m256 vpim = _mm256_load_ps(pim_arr);
            __m256 zre = p.julia ? vpre : vzero;
            __m256 zim = p.julia ? vpim : vzero;
            const __m256 cre = p.julia ? vjre : vpre;
            const __m256 cim = p.julia ? vjim : vpim;
            __m256 zre2 = _mm256_mul_ps(zre, zre);
            __m256 zim2 = _mm256_mul_ps(zim, zim);

            int iter_arr[8] = {
                p.iterations, p.iterations, p.iterations, p.iterations,
                p.iterations, p.iterations, p.iterations, p.iterations
            };
            int active = valid;
            for (int iter = 0; iter < p.iterations && active; ++iter) {
                __m256 nre, nim;
                step_variant_ps(variant, zre, zim, zre2, zim2, cre, cim, nre, nim);
                const __m256 nre2 = _mm256_mul_ps(nre, nre);
                const __m256 nim2 = _mm256_mul_ps(nim, nim);
                const __m256 norm2 = _mm256_add_ps(nre2, nim2);
                const __m256 escaped_vec = _mm256_and_ps(
                    valid_vec,
                    _mm256_or_ps(
                        _mm256_cmp_ps(norm2, vbail2, _CMP_GT_OQ),
                        _mm256_cmp_ps(norm2, norm2, _CMP_UNORD_Q)));
                const int escaped = _mm256_movemask_ps(escaped_vec) & active;
                if (escaped) {
                    for (int lane = 0; lane < 8; ++lane) {
                        if (escaped & (1 << lane)) iter_arr[lane] = iter;
                    }
                    active &= ~escaped;
                }
                const __m256 active_vec = lane_mask_ps(active);
                zre = _mm256_blendv_ps(zre, nre, active_vec);
                zim = _mm256_blendv_ps(zim, nim, active_vec);
                zre2 = _mm256_blendv_ps(zre2, nre2, active_vec);
                zim2 = _mm256_blendv_ps(zim2, nim2, active_vec);
            }

            for (int lane = 0; lane < 8 && col + lane < s; ++lane) {
                uint8_t* px = rowp + 3 * (col + lane);
                colorize_escape_bgr(iter_arr[lane], p.iterations, p.colormap, 0.0, false, px[0], px[1], px[2]);
            }
        }

        if (on_row_done) {
            const int done = rows_done.fetch_add(1, std::memory_order_relaxed) + 1;
            if (done == row_end - row_start || (done % 16) == 0) on_row_done(done);
        }
    };

    if (threaded) {
        const int thread_count = default_render_threads();
        #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 4)
        for (int row = row_start; row < row_end; ++row) {
            render_row(row);
        }
    } else {
        for (int row = row_start; row < row_end; ++row) {
            render_row(row);
        }
    }
}

} // namespace
#endif

bool ln_map_avx2_available() noexcept {
#if defined(__AVX2__) && defined(__FMA__)
    return avx2_available() && fma_available();
#else
    return false;
#endif
}

LnMapStats render_ln_map_avx2_rows(const LnMapParams& p, cv::Mat& out, int row_start, int row_count, const LnMapProgress& on_row_done) {
    if (!ln_map_avx2_available() || !ln_map_variant_supported_by_simd(p.variant)) {
        return render_ln_map_openmp_rows(p, out, row_start, row_count, on_row_done);
    }

#if defined(__AVX2__) && defined(__FMA__)
    ensure_out(p, out);
    const auto [start, end] = clamp_rows(p, row_start, row_count);
    const auto t0 = std::chrono::steady_clock::now();
    render_rows_impl(p, out, start, end, false, on_row_done);
    const auto t1 = std::chrono::steady_clock::now();

    LnMapStats stats;
    stats.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    stats.pixel_count = p.width_s * (end - start);
    stats.engine_used = "avx2";
    stats.scalar_used = "fp64";
    return stats;
#else
    return render_ln_map_openmp_rows(p, out, row_start, row_count, on_row_done);
#endif
}

LnMapStats render_ln_map_avx2_fp32_rows(const LnMapParams& p, cv::Mat& out, int row_start, int row_count, const LnMapProgress& on_row_done) {
    if (!ln_map_avx2_available() || !ln_map_variant_supported_by_simd(p.variant)) {
        return render_ln_map_openmp_rows(p, out, row_start, row_count, on_row_done);
    }

#if defined(__AVX2__) && defined(__FMA__)
    ensure_out(p, out);
    const auto [start, end] = clamp_rows(p, row_start, row_count);
    const auto t0 = std::chrono::steady_clock::now();
    render_rows_fp32_impl(p, out, start, end, false, on_row_done);
    const auto t1 = std::chrono::steady_clock::now();

    LnMapStats stats;
    stats.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    stats.pixel_count = p.width_s * (end - start);
    stats.engine_used = "avx2";
    stats.scalar_used = "fp32";
    stats.precision_mode = "fast";
    return stats;
#else
    return render_ln_map_openmp_rows(p, out, row_start, row_count, on_row_done);
#endif
}

LnMapStats render_ln_map_avx2(const LnMapParams& p, cv::Mat& out, const LnMapProgress& on_row_done) {
    if (!ln_map_avx2_available() || !ln_map_variant_supported_by_simd(p.variant)) {
        return render_ln_map_openmp(p, out, on_row_done);
    }

#if defined(__AVX2__) && defined(__FMA__)
    ensure_out(p, out);
    const auto t0 = std::chrono::steady_clock::now();
    render_rows_impl(p, out, 0, p.height_t, true, on_row_done);
    const auto t1 = std::chrono::steady_clock::now();

    LnMapStats stats;
    stats.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    stats.pixel_count = p.width_s * p.height_t;
    stats.engine_used = "avx2";
    stats.scalar_used = "fp64";
    return stats;
#else
    return render_ln_map_openmp(p, out, on_row_done);
#endif
}

} // namespace fsd::compute
