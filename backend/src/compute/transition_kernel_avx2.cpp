// compute/transition_kernel_avx2.cpp
//
// AVX2 accelerated 2D transition slice renderer.
// Follows the FoldRules pattern from transition_volume_avx2.cpp.

#include "transition_kernel_avx2.hpp"
#include "cpu_features.hpp"
#include "parallel.hpp"
#include "variants.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#if defined(__AVX2__) && defined(__FMA__)
#  include <immintrin.h>
#endif

namespace fsd::compute {

#if defined(__AVX2__) && defined(__FMA__)

namespace {

// ── FoldRules (shared pattern with transition_volume_avx2.cpp) ──────────────

struct FoldRules {
    bool post_abs_real = false;
    bool abs_x         = false;
    bool abs_axis      = false;
    bool neg_imag      = false;
};

FoldRules fold_rules(Variant v) {
    FoldRules r;
    r.post_abs_real =
        v == Variant::Fish || v == Variant::Vase || v == Variant::Bird ||
        v == Variant::Mask || v == Variant::Ship;
    r.abs_x = v == Variant::Boat || v == Variant::Bell || v == Variant::Ship;
    r.abs_axis = v == Variant::Boat || v == Variant::Duck ||
                 v == Variant::Mask || v == Variant::Bird;
    r.neg_imag = v == Variant::Tri || v == Variant::Bell ||
                 v == Variant::Vase || v == Variant::Ship;
    return r;
}

// ── AVX2 fp64 helpers ───────────────────────────────────────────────────────

inline __m256d avx2_abs_pd(__m256d v) {
    const __m256d sign = _mm256_set1_pd(-0.0);
    return _mm256_andnot_pd(sign, v);
}

inline __m256d real_projection_pd(__m256d x2, __m256d axis2, const FoldRules& r) {
    __m256d q = _mm256_sub_pd(x2, axis2);
    return r.post_abs_real ? avx2_abs_pd(q) : q;
}

inline __m256d imag_projection_pd(__m256d x, __m256d axis, const FoldRules& r) {
    const __m256d a = r.abs_x    ? avx2_abs_pd(x)    : x;
    const __m256d b = r.abs_axis ? avx2_abs_pd(axis)  : axis;
    __m256d q = _mm256_add_pd(_mm256_mul_pd(a, b), _mm256_mul_pd(a, b));
    return r.neg_imag ? _mm256_sub_pd(_mm256_setzero_pd(), q) : q;
}

// ── Cancellation helper ─────────────────────────────────────────────────────

inline bool should_cancel(const TransitionParams& p, std::atomic<bool>& cancelled) {
    if (cancelled.load(std::memory_order_relaxed)) return true;
    if (p.base.should_cancel && p.base.should_cancel()) {
        cancelled.store(true, std::memory_order_relaxed);
        return true;
    }
    return false;
}

constexpr int MAX_TRANSITION_LEGS = 4;

struct ActiveAvxLeg {
    FoldRules rules;
    double direction = 1.0;
    double influence = 1.0;
};

bool active_avx_legs(const std::vector<TransitionLeg>& input, ActiveAvxLeg (&out)[MAX_TRANSITION_LEGS], int& count) {
    if (input.size() > MAX_TRANSITION_LEGS) return false;
    double max_w = 0.0;
    double sum_w2 = 0.0;
    TransitionLeg kept[MAX_TRANSITION_LEGS];
    int kept_count = 0;
    for (const TransitionLeg& leg : input) {
        if (!variant_supports_axis_transition(leg.variant)) return false;
        if (!std::isfinite(leg.weight)) return false;
        if (leg.weight <= 0.0) continue;
        if (kept_count >= MAX_TRANSITION_LEGS) return false;
        kept[kept_count++] = leg;
        max_w = std::max(max_w, leg.weight);
        sum_w2 += leg.weight * leg.weight;
    }
    if (kept_count <= 0 || max_w <= 0.0 || sum_w2 <= 0.0) return false;
    const double inv_len = 1.0 / std::sqrt(sum_w2);
    count = kept_count;
    for (int i = 0; i < kept_count; ++i) {
        out[i] = {
            fold_rules(kept[i].variant),
            kept[i].weight * inv_len,
            kept[i].weight / max_w,
        };
    }
    return true;
}

// ── Escape metric — fp64, 4 pixels per vector ──────────────────────────────

MapStats render_escape_fp64(const TransitionParams& p, FieldOutput& fo) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto& b = p.base;
    const int W = b.width, H = b.height;
    const size_t npx = static_cast<size_t>(W) * H;

    fo.width = W; fo.height = H; fo.metric = Metric::Escape;
    fo.iter_u32.resize(npx);
    fo.norm_f32.resize(npx);

    const double aspect  = static_cast<double>(W) / H;
    const double span_im = b.scale;
    const double span_re = b.scale * aspect;
    const double re_min  = b.center_re - span_re * 0.5;
    const double im_max  = b.center_im + span_im * 0.5;
    const double bail2   = b.bailout_sq;
    const double cth     = std::cos(p.theta);
    const double sth     = std::sin(p.theta);

    const FoldRules from = fold_rules(p.from_variant);
    const FoldRules to   = fold_rules(p.to_variant);

    const __m256d vbail2   = _mm256_set1_pd(bail2);
    const __m256d v_cth    = _mm256_set1_pd(cth);
    const __m256d v_sth    = _mm256_set1_pd(sth);
    const __m256d v_re_min = _mm256_set1_pd(re_min);
    const __m256d v_span_re= _mm256_set1_pd(span_re);
    const __m256d v_inv_W  = _mm256_set1_pd(1.0 / W);
    const __m256d v_julia_re = _mm256_set1_pd(b.julia_re);
    const __m256d v_julia_cy = _mm256_set1_pd(b.julia_im * cth);
    const __m256d v_julia_cz = _mm256_set1_pd(b.julia_im * sth);
    const __m256d lane_offsets = _mm256_set_pd(3.5, 2.5, 1.5, 0.5);

    const int thread_count = resolve_render_threads(b.render_threads);
    std::atomic<bool> cancelled{false};

    #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 4)
    for (int y = 0; y < H; y++) {
        if (should_cancel(p, cancelled)) continue;
        const double v_raw = im_max - (static_cast<double>(y) + 0.5) / H * span_im;
        const __m256d vy_raw = _mm256_set1_pd(v_raw);
        const __m256d vy0 = _mm256_mul_pd(vy_raw, v_cth);
        const __m256d vz0 = _mm256_mul_pd(vy_raw, v_sth);

        const size_t row_off = static_cast<size_t>(y) * W;

        for (int x = 0; x < W; x += 4) {
            const int remaining = std::min(4, W - x);

            const __m256d vxi = _mm256_add_pd(_mm256_set1_pd(static_cast<double>(x)), lane_offsets);
            const __m256d vu  = _mm256_fmadd_pd(
                _mm256_mul_pd(vxi, v_inv_W), v_span_re, v_re_min);

            __m256d vx0 = vu;
            __m256d vcx, vcy, vcz;
            if (b.julia) {
                vcx = v_julia_re; vcy = v_julia_cy; vcz = v_julia_cz;
            } else {
                vcx = vx0; vcy = vy0; vcz = vz0;
            }

            __m256d vx = vx0, vy = vy0, vz = vz0;
            __m256d vx2 = _mm256_mul_pd(vx, vx);
            __m256d vy2 = _mm256_mul_pd(vy, vy);
            __m256d vz2 = _mm256_mul_pd(vz, vz);
            __m256d vnorm = _mm256_setzero_pd();

            int active = (1 << remaining) - 1;
            alignas(32) int iters[4] = { b.iterations, b.iterations, b.iterations, b.iterations };

            for (int i = 0; i < b.iterations && active; i++) {
                const __m256d nx = _mm256_add_pd(
                    _mm256_sub_pd(
                        _mm256_add_pd(real_projection_pd(vx2, vy2, from),
                                      real_projection_pd(vx2, vz2, to)),
                        vx2),
                    vcx);
                const __m256d ny = _mm256_add_pd(imag_projection_pd(vx, vy, from), vcy);
                const __m256d nz = _mm256_add_pd(imag_projection_pd(vx, vz, to),   vcz);

                const __m256d nx2 = _mm256_mul_pd(nx, nx);
                const __m256d ny2 = _mm256_mul_pd(ny, ny);
                const __m256d nz2 = _mm256_mul_pd(nz, nz);
                const __m256d n2  = _mm256_add_pd(nx2, _mm256_add_pd(ny2, nz2));

                const __m256d esc_r = _mm256_cmp_pd(n2, vbail2, _CMP_GT_OQ);
                const __m256d esc_n = _mm256_cmp_pd(n2, n2, _CMP_UNORD_Q);
                const int escaped = _mm256_movemask_pd(_mm256_or_pd(esc_r, esc_n)) & active;
                if (escaped) {
                    for (int k = 0; k < 4; k++) {
                        if (escaped & (1 << k)) iters[k] = i;
                    }
                    active &= ~escaped;
                    vnorm = _mm256_blendv_pd(vnorm, n2,
                        _mm256_castsi256_pd(_mm256_set_epi64x(
                            (escaped & 8) ? -1LL : 0LL,
                            (escaped & 4) ? -1LL : 0LL,
                            (escaped & 2) ? -1LL : 0LL,
                            (escaped & 1) ? -1LL : 0LL)));
                }

                const __m256d active_mask = _mm256_castsi256_pd(_mm256_set_epi64x(
                    (active & 8) ? -1LL : 0LL,
                    (active & 4) ? -1LL : 0LL,
                    (active & 2) ? -1LL : 0LL,
                    (active & 1) ? -1LL : 0LL));
                vx = _mm256_blendv_pd(vx, nx, active_mask);
                vy = _mm256_blendv_pd(vy, ny, active_mask);
                vz = _mm256_blendv_pd(vz, nz, active_mask);
                vx2 = _mm256_blendv_pd(vx2, nx2, active_mask);
                vy2 = _mm256_blendv_pd(vy2, ny2, active_mask);
                vz2 = _mm256_blendv_pd(vz2, nz2, active_mask);
            }

            alignas(32) double norms[4];
            _mm256_store_pd(norms, vnorm);
            for (int k = 0; k < remaining; k++) {
                const size_t idx = row_off + static_cast<size_t>(x + k);
                fo.iter_u32[idx] = static_cast<uint32_t>(iters[k]);
                fo.norm_f32[idx] = (iters[k] < b.iterations)
                    ? static_cast<float>(norms[k]) : 0.0f;
            }
        }
    }

    if (cancelled.load(std::memory_order_relaxed))
        throw std::runtime_error("transition rendering cancelled");

    const auto t1 = std::chrono::steady_clock::now();
    MapStats s;
    s.elapsed_ms  = std::chrono::duration<double, std::milli>(t1 - t0).count();
    s.pixel_count = W * H;
    s.scalar_used = "fp64";
    s.engine_used = "avx2";
    fo.scalar_used = s.scalar_used;
    fo.engine_used = s.engine_used;
    fo.elapsed_ms  = s.elapsed_ms;
    return s;
}

// ── Non-escape metrics — fp64, 4 pixels per vector ──────────────────────────

MapStats render_metric_fp64(const TransitionParams& p, FieldOutput& fo) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto& b = p.base;
    const int W = b.width, H = b.height;
    const size_t npx = static_cast<size_t>(W) * H;

    fo.width = W; fo.height = H; fo.metric = b.metric;
    fo.field_f64.resize(npx);

    const double aspect  = static_cast<double>(W) / H;
    const double span_im = b.scale;
    const double span_re = b.scale * aspect;
    const double re_min  = b.center_re - span_re * 0.5;
    const double im_max  = b.center_im + span_im * 0.5;
    const double bail2   = b.bailout_sq;
    const double cth     = std::cos(p.theta);
    const double sth     = std::sin(p.theta);

    const FoldRules from = fold_rules(p.from_variant);
    const FoldRules to   = fold_rules(p.to_variant);

    const __m256d vbail2 = _mm256_set1_pd(bail2);
    const __m256d v_cth  = _mm256_set1_pd(cth);
    const __m256d v_sth  = _mm256_set1_pd(sth);
    const __m256d v_re_min = _mm256_set1_pd(re_min);
    const __m256d v_span_re = _mm256_set1_pd(span_re);
    const __m256d v_inv_W   = _mm256_set1_pd(1.0 / W);
    const __m256d v_julia_re = _mm256_set1_pd(b.julia_re);
    const __m256d v_julia_cy = _mm256_set1_pd(b.julia_im * cth);
    const __m256d v_julia_cz = _mm256_set1_pd(b.julia_im * sth);
    const __m256d lane_offsets = _mm256_set_pd(3.5, 2.5, 1.5, 0.5);

    const bool track_min = (b.metric == Metric::MinAbs || b.metric == Metric::Envelope);
    const bool track_max = (b.metric == Metric::MaxAbs || b.metric == Metric::Envelope);
    const int thread_count = resolve_render_threads(b.render_threads);

    double global_min =  std::numeric_limits<double>::infinity();
    double global_max = -std::numeric_limits<double>::infinity();
    std::atomic<bool> cancelled{false};

    #pragma omp parallel num_threads(thread_count) reduction(min:global_min) reduction(max:global_max)
    {
    #pragma omp for schedule(dynamic, 4)
    for (int y = 0; y < H; y++) {
        if (should_cancel(p, cancelled)) continue;
        const double v_raw = im_max - (static_cast<double>(y) + 0.5) / H * span_im;
        const __m256d vy_raw = _mm256_set1_pd(v_raw);
        const __m256d vy0 = _mm256_mul_pd(vy_raw, v_cth);
        const __m256d vz0 = _mm256_mul_pd(vy_raw, v_sth);
        const size_t row_off = static_cast<size_t>(y) * W;

        for (int x = 0; x < W; x += 4) {
            const int remaining = std::min(4, W - x);

            const __m256d vxi = _mm256_add_pd(_mm256_set1_pd(static_cast<double>(x)), lane_offsets);
            const __m256d vu  = _mm256_fmadd_pd(
                _mm256_mul_pd(vxi, v_inv_W), v_span_re, v_re_min);

            __m256d vx0 = vu;
            __m256d vcx, vcy, vcz;
            if (b.julia) {
                vcx = v_julia_re; vcy = v_julia_cy; vcz = v_julia_cz;
            } else {
                vcx = vx0; vcy = vy0; vcz = vz0;
            }

            __m256d vx = vx0, vy = vy0, vz = vz0;
            __m256d vx2 = _mm256_mul_pd(vx, vx);
            __m256d vy2 = _mm256_mul_pd(vy, vy);
            __m256d vz2 = _mm256_mul_pd(vz, vz);

            __m256d init_n2 = _mm256_add_pd(vx2, _mm256_add_pd(vy2, vz2));
            __m256d vmin_sq = init_n2;
            __m256d vmax_sq = init_n2;

            int active = (1 << remaining) - 1;

            for (int i = 0; i < b.iterations && active; i++) {
                const __m256d nx = _mm256_add_pd(
                    _mm256_sub_pd(
                        _mm256_add_pd(real_projection_pd(vx2, vy2, from),
                                      real_projection_pd(vx2, vz2, to)),
                        vx2),
                    vcx);
                const __m256d ny = _mm256_add_pd(imag_projection_pd(vx, vy, from), vcy);
                const __m256d nz = _mm256_add_pd(imag_projection_pd(vx, vz, to),   vcz);

                const __m256d nx2 = _mm256_mul_pd(nx, nx);
                const __m256d ny2 = _mm256_mul_pd(ny, ny);
                const __m256d nz2 = _mm256_mul_pd(nz, nz);
                const __m256d n2  = _mm256_add_pd(nx2, _mm256_add_pd(ny2, nz2));

                if (track_min) vmin_sq = _mm256_min_pd(vmin_sq, n2);
                if (track_max) vmax_sq = _mm256_max_pd(vmax_sq, n2);

                const __m256d esc_r = _mm256_cmp_pd(n2, vbail2, _CMP_GT_OQ);
                const __m256d esc_n = _mm256_cmp_pd(n2, n2, _CMP_UNORD_Q);
                const int escaped = _mm256_movemask_pd(_mm256_or_pd(esc_r, esc_n)) & active;
                if (escaped) active &= ~escaped;

                const __m256d active_mask = _mm256_castsi256_pd(_mm256_set_epi64x(
                    (active & 8) ? -1LL : 0LL,
                    (active & 4) ? -1LL : 0LL,
                    (active & 2) ? -1LL : 0LL,
                    (active & 1) ? -1LL : 0LL));
                vx = _mm256_blendv_pd(vx, nx, active_mask);
                vy = _mm256_blendv_pd(vy, ny, active_mask);
                vz = _mm256_blendv_pd(vz, nz, active_mask);
                vx2 = _mm256_blendv_pd(vx2, nx2, active_mask);
                vy2 = _mm256_blendv_pd(vy2, ny2, active_mask);
                vz2 = _mm256_blendv_pd(vz2, nz2, active_mask);
            }

            alignas(32) double min_arr[4], max_arr[4];
            _mm256_store_pd(min_arr, vmin_sq);
            _mm256_store_pd(max_arr, vmax_sq);
            for (int k = 0; k < remaining; k++) {
                double fv = 0.0;
                if (b.metric == Metric::MinAbs) fv = std::sqrt(min_arr[k]);
                else if (b.metric == Metric::MaxAbs) fv = std::sqrt(max_arr[k]);
                else if (b.metric == Metric::Envelope) fv = 0.5 * (std::sqrt(min_arr[k]) + std::sqrt(max_arr[k]));
                const size_t idx = row_off + static_cast<size_t>(x + k);
                fo.field_f64[idx] = fv;
                if (fv < global_min) global_min = fv;
                if (fv > global_max) global_max = fv;
            }
        }
    }
    } // end omp parallel

    if (cancelled.load(std::memory_order_relaxed))
        throw std::runtime_error("transition rendering cancelled");

    fo.field_min = global_min;
    fo.field_max = global_max;

    const auto t1 = std::chrono::steady_clock::now();
    MapStats s;
    s.elapsed_ms  = std::chrono::duration<double, std::milli>(t1 - t0).count();
    s.pixel_count = W * H;
    s.scalar_used = "fp64";
    s.engine_used = "avx2";
    fo.scalar_used = s.scalar_used;
    fo.engine_used = s.engine_used;
    fo.elapsed_ms  = s.elapsed_ms;
    return s;
}

MapStats render_multi_escape_fp64(const TransitionParams& p, FieldOutput& fo) {
    ActiveAvxLeg legs[MAX_TRANSITION_LEGS];
    int leg_count = 0;
    if (!active_avx_legs(p.multi_legs, legs, leg_count)) {
        throw std::runtime_error("invalid AVX2 multi transition legs");
    }

    const auto t0 = std::chrono::steady_clock::now();
    const auto& b = p.base;
    const int W = b.width, H = b.height;
    const size_t npx = static_cast<size_t>(W) * H;

    fo.width = W; fo.height = H; fo.metric = Metric::Escape;
    fo.iter_u32.resize(npx);
    fo.norm_f32.resize(npx);

    const double aspect  = static_cast<double>(W) / H;
    const double span_im = b.scale;
    const double span_re = b.scale * aspect;
    const double re_min  = b.center_re - span_re * 0.5;
    const double im_max  = b.center_im + span_im * 0.5;
    const double bail2   = b.bailout_sq;

    const __m256d vbail2    = _mm256_set1_pd(bail2);
    const __m256d v_re_min  = _mm256_set1_pd(re_min);
    const __m256d v_span_re = _mm256_set1_pd(span_re);
    const __m256d v_inv_W   = _mm256_set1_pd(1.0 / W);
    const __m256d v_julia_re = _mm256_set1_pd(b.julia_re);
    const __m256d lane_offsets = _mm256_set_pd(3.5, 2.5, 1.5, 0.5);

    const int thread_count = resolve_render_threads(b.render_threads);
    std::atomic<bool> cancelled{false};

    #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 4)
    for (int y = 0; y < H; y++) {
        if (should_cancel(p, cancelled)) continue;
        const double v_raw = im_max - (static_cast<double>(y) + 0.5) / H * span_im;
        const __m256d vv_raw = _mm256_set1_pd(v_raw);
        const size_t row_off = static_cast<size_t>(y) * W;

        for (int x = 0; x < W; x += 4) {
            const int remaining = std::min(4, W - x);
            const __m256d vxi = _mm256_add_pd(_mm256_set1_pd(static_cast<double>(x)), lane_offsets);
            const __m256d vu  = _mm256_fmadd_pd(
                _mm256_mul_pd(vxi, v_inv_W), v_span_re, v_re_min);

            __m256d vx = vu;
            __m256d vx2 = _mm256_mul_pd(vx, vx);
            __m256d vcx = b.julia ? v_julia_re : vu;
            __m256d vaxis[MAX_TRANSITION_LEGS];
            __m256d vaxis2[MAX_TRANSITION_LEGS];
            __m256d vcaxis[MAX_TRANSITION_LEGS];
            for (int i = 0; i < leg_count; ++i) {
                const __m256d dir = _mm256_set1_pd(legs[i].direction);
                vaxis[i] = _mm256_mul_pd(vv_raw, dir);
                vaxis2[i] = _mm256_mul_pd(vaxis[i], vaxis[i]);
                vcaxis[i] = b.julia
                    ? _mm256_set1_pd(b.julia_im * legs[i].direction)
                    : vaxis[i];
            }

            __m256d vnorm = _mm256_setzero_pd();
            int active = (1 << remaining) - 1;
            alignas(32) int iters[4] = { b.iterations, b.iterations, b.iterations, b.iterations };

            for (int iter = 0; iter < b.iterations && active; ++iter) {
                __m256d real_sum = _mm256_setzero_pd();
                double influence_sum_s = 0.0;
                __m256d next_axis[MAX_TRANSITION_LEGS];
                for (int i = 0; i < leg_count; ++i) {
                    const __m256d inf = _mm256_set1_pd(legs[i].influence);
                    real_sum = _mm256_add_pd(real_sum,
                        _mm256_mul_pd(inf, real_projection_pd(vx2, vaxis2[i], legs[i].rules)));
                    influence_sum_s += legs[i].influence;
                    next_axis[i] = _mm256_add_pd(
                        _mm256_mul_pd(inf, imag_projection_pd(vx, vaxis[i], legs[i].rules)),
                        vcaxis[i]);
                }

                const __m256d nx = _mm256_add_pd(
                    _mm256_sub_pd(real_sum, _mm256_mul_pd(_mm256_set1_pd(influence_sum_s - 1.0), vx2)),
                    vcx);
                const __m256d nx2 = _mm256_mul_pd(nx, nx);
                __m256d n2 = nx2;
                for (int i = 0; i < leg_count; ++i) {
                    n2 = _mm256_add_pd(n2, _mm256_mul_pd(next_axis[i], next_axis[i]));
                }

                const __m256d esc_r = _mm256_cmp_pd(n2, vbail2, _CMP_GT_OQ);
                const __m256d esc_n = _mm256_cmp_pd(n2, n2, _CMP_UNORD_Q);
                const int escaped = _mm256_movemask_pd(_mm256_or_pd(esc_r, esc_n)) & active;
                if (escaped) {
                    for (int k = 0; k < 4; ++k) {
                        if (escaped & (1 << k)) iters[k] = iter;
                    }
                    active &= ~escaped;
                    vnorm = _mm256_blendv_pd(vnorm, n2,
                        _mm256_castsi256_pd(_mm256_set_epi64x(
                            (escaped & 8) ? -1LL : 0LL,
                            (escaped & 4) ? -1LL : 0LL,
                            (escaped & 2) ? -1LL : 0LL,
                            (escaped & 1) ? -1LL : 0LL)));
                }

                const __m256d active_mask = _mm256_castsi256_pd(_mm256_set_epi64x(
                    (active & 8) ? -1LL : 0LL,
                    (active & 4) ? -1LL : 0LL,
                    (active & 2) ? -1LL : 0LL,
                    (active & 1) ? -1LL : 0LL));
                vx = _mm256_blendv_pd(vx, nx, active_mask);
                vx2 = _mm256_blendv_pd(vx2, nx2, active_mask);
                for (int i = 0; i < leg_count; ++i) {
                    const __m256d na2 = _mm256_mul_pd(next_axis[i], next_axis[i]);
                    vaxis[i] = _mm256_blendv_pd(vaxis[i], next_axis[i], active_mask);
                    vaxis2[i] = _mm256_blendv_pd(vaxis2[i], na2, active_mask);
                }
            }

            alignas(32) double norms[4];
            _mm256_store_pd(norms, vnorm);
            for (int k = 0; k < remaining; ++k) {
                const size_t idx = row_off + static_cast<size_t>(x + k);
                fo.iter_u32[idx] = static_cast<uint32_t>(iters[k]);
                fo.norm_f32[idx] = (iters[k] < b.iterations)
                    ? static_cast<float>(norms[k]) : 0.0f;
            }
        }
    }

    if (cancelled.load(std::memory_order_relaxed))
        throw std::runtime_error("transition rendering cancelled");

    const auto t1 = std::chrono::steady_clock::now();
    MapStats s;
    s.elapsed_ms  = std::chrono::duration<double, std::milli>(t1 - t0).count();
    s.pixel_count = W * H;
    s.scalar_used = "fp64";
    s.engine_used = "avx2_multi";
    fo.scalar_used = s.scalar_used;
    fo.engine_used = s.engine_used;
    fo.elapsed_ms  = s.elapsed_ms;
    return s;
}

MapStats render_multi_metric_fp64(const TransitionParams& p, FieldOutput& fo) {
    ActiveAvxLeg legs[MAX_TRANSITION_LEGS];
    int leg_count = 0;
    if (!active_avx_legs(p.multi_legs, legs, leg_count)) {
        throw std::runtime_error("invalid AVX2 multi transition legs");
    }

    const auto t0 = std::chrono::steady_clock::now();
    const auto& b = p.base;
    const int W = b.width, H = b.height;
    const size_t npx = static_cast<size_t>(W) * H;

    fo.width = W; fo.height = H; fo.metric = b.metric;
    fo.field_f64.resize(npx);

    const double aspect  = static_cast<double>(W) / H;
    const double span_im = b.scale;
    const double span_re = b.scale * aspect;
    const double re_min  = b.center_re - span_re * 0.5;
    const double im_max  = b.center_im + span_im * 0.5;
    const double bail2   = b.bailout_sq;

    const __m256d vbail2    = _mm256_set1_pd(bail2);
    const __m256d v_re_min  = _mm256_set1_pd(re_min);
    const __m256d v_span_re = _mm256_set1_pd(span_re);
    const __m256d v_inv_W   = _mm256_set1_pd(1.0 / W);
    const __m256d v_julia_re = _mm256_set1_pd(b.julia_re);
    const __m256d lane_offsets = _mm256_set_pd(3.5, 2.5, 1.5, 0.5);

    const bool track_min = (b.metric == Metric::MinAbs || b.metric == Metric::Envelope);
    const bool track_max = (b.metric == Metric::MaxAbs || b.metric == Metric::Envelope);
    const int thread_count = resolve_render_threads(b.render_threads);

    double global_min =  std::numeric_limits<double>::infinity();
    double global_max = -std::numeric_limits<double>::infinity();
    std::atomic<bool> cancelled{false};

    #pragma omp parallel num_threads(thread_count) reduction(min:global_min) reduction(max:global_max)
    {
    #pragma omp for schedule(dynamic, 4)
    for (int y = 0; y < H; y++) {
        if (should_cancel(p, cancelled)) continue;
        const double v_raw = im_max - (static_cast<double>(y) + 0.5) / H * span_im;
        const __m256d vv_raw = _mm256_set1_pd(v_raw);
        const size_t row_off = static_cast<size_t>(y) * W;

        for (int x = 0; x < W; x += 4) {
            const int remaining = std::min(4, W - x);
            const __m256d vxi = _mm256_add_pd(_mm256_set1_pd(static_cast<double>(x)), lane_offsets);
            const __m256d vu  = _mm256_fmadd_pd(
                _mm256_mul_pd(vxi, v_inv_W), v_span_re, v_re_min);

            __m256d vx = vu;
            __m256d vx2 = _mm256_mul_pd(vx, vx);
            __m256d vcx = b.julia ? v_julia_re : vu;
            __m256d vaxis[MAX_TRANSITION_LEGS];
            __m256d vaxis2[MAX_TRANSITION_LEGS];
            __m256d vcaxis[MAX_TRANSITION_LEGS];
            __m256d init_n2 = vx2;
            for (int i = 0; i < leg_count; ++i) {
                const __m256d dir = _mm256_set1_pd(legs[i].direction);
                vaxis[i] = _mm256_mul_pd(vv_raw, dir);
                vaxis2[i] = _mm256_mul_pd(vaxis[i], vaxis[i]);
                vcaxis[i] = b.julia
                    ? _mm256_set1_pd(b.julia_im * legs[i].direction)
                    : vaxis[i];
                init_n2 = _mm256_add_pd(init_n2, vaxis2[i]);
            }

            __m256d vmin_sq = init_n2;
            __m256d vmax_sq = init_n2;
            int active = (1 << remaining) - 1;

            for (int iter = 0; iter < b.iterations && active; ++iter) {
                __m256d real_sum = _mm256_setzero_pd();
                double influence_sum_s = 0.0;
                __m256d next_axis[MAX_TRANSITION_LEGS];
                for (int i = 0; i < leg_count; ++i) {
                    const __m256d inf = _mm256_set1_pd(legs[i].influence);
                    real_sum = _mm256_add_pd(real_sum,
                        _mm256_mul_pd(inf, real_projection_pd(vx2, vaxis2[i], legs[i].rules)));
                    influence_sum_s += legs[i].influence;
                    next_axis[i] = _mm256_add_pd(
                        _mm256_mul_pd(inf, imag_projection_pd(vx, vaxis[i], legs[i].rules)),
                        vcaxis[i]);
                }

                const __m256d nx = _mm256_add_pd(
                    _mm256_sub_pd(real_sum, _mm256_mul_pd(_mm256_set1_pd(influence_sum_s - 1.0), vx2)),
                    vcx);
                const __m256d nx2 = _mm256_mul_pd(nx, nx);
                __m256d n2 = nx2;
                for (int i = 0; i < leg_count; ++i) {
                    n2 = _mm256_add_pd(n2, _mm256_mul_pd(next_axis[i], next_axis[i]));
                }

                if (track_min) vmin_sq = _mm256_min_pd(vmin_sq, n2);
                if (track_max) vmax_sq = _mm256_max_pd(vmax_sq, n2);

                const __m256d esc_r = _mm256_cmp_pd(n2, vbail2, _CMP_GT_OQ);
                const __m256d esc_n = _mm256_cmp_pd(n2, n2, _CMP_UNORD_Q);
                const int escaped = _mm256_movemask_pd(_mm256_or_pd(esc_r, esc_n)) & active;
                if (escaped) active &= ~escaped;

                const __m256d active_mask = _mm256_castsi256_pd(_mm256_set_epi64x(
                    (active & 8) ? -1LL : 0LL,
                    (active & 4) ? -1LL : 0LL,
                    (active & 2) ? -1LL : 0LL,
                    (active & 1) ? -1LL : 0LL));
                vx = _mm256_blendv_pd(vx, nx, active_mask);
                vx2 = _mm256_blendv_pd(vx2, nx2, active_mask);
                for (int i = 0; i < leg_count; ++i) {
                    const __m256d na2 = _mm256_mul_pd(next_axis[i], next_axis[i]);
                    vaxis[i] = _mm256_blendv_pd(vaxis[i], next_axis[i], active_mask);
                    vaxis2[i] = _mm256_blendv_pd(vaxis2[i], na2, active_mask);
                }
            }

            alignas(32) double min_arr[4], max_arr[4];
            _mm256_store_pd(min_arr, vmin_sq);
            _mm256_store_pd(max_arr, vmax_sq);
            for (int k = 0; k < remaining; ++k) {
                double fv = 0.0;
                if (b.metric == Metric::MinAbs) fv = std::sqrt(min_arr[k]);
                else if (b.metric == Metric::MaxAbs) fv = std::sqrt(max_arr[k]);
                else if (b.metric == Metric::Envelope) fv = 0.5 * (std::sqrt(min_arr[k]) + std::sqrt(max_arr[k]));
                const size_t idx = row_off + static_cast<size_t>(x + k);
                fo.field_f64[idx] = fv;
                if (fv < global_min) global_min = fv;
                if (fv > global_max) global_max = fv;
            }
        }
    }
    } // end omp parallel

    if (cancelled.load(std::memory_order_relaxed))
        throw std::runtime_error("transition rendering cancelled");

    fo.field_min = global_min;
    fo.field_max = global_max;

    const auto t1 = std::chrono::steady_clock::now();
    MapStats s;
    s.elapsed_ms  = std::chrono::duration<double, std::milli>(t1 - t0).count();
    s.pixel_count = W * H;
    s.scalar_used = "fp64";
    s.engine_used = "avx2_multi";
    fo.scalar_used = s.scalar_used;
    fo.engine_used = s.engine_used;
    fo.elapsed_ms  = s.elapsed_ms;
    return s;
}

} // namespace

bool render_transition_field_avx2(const TransitionParams& p, FieldOutput& fo) {
    if (!avx2_available() || !fma_available()) return false;
    if (!p.multi_legs.empty()) {
        if (p.base.metric == Metric::MinPairwiseDist) return false;
        if (p.base.metric == Metric::Escape) render_multi_escape_fp64(p, fo);
        else render_multi_metric_fp64(p, fo);
        return true;
    }
    if (!variant_supports_axis_transition(p.from_variant) ||
        !variant_supports_axis_transition(p.to_variant)) return false;
    if (p.base.metric == Metric::MinPairwiseDist) return false;

    if (p.base.metric == Metric::Escape) {
        render_escape_fp64(p, fo);
    } else {
        render_metric_fp64(p, fo);
    }
    return true;
}

#else // no AVX2+FMA

bool render_transition_field_avx2(const TransitionParams&, FieldOutput&) {
    return false;
}

#endif

} // namespace fsd::compute
