// compute/transition_volume_avx2.cpp

#include "transition_volume_avx2.hpp"

#include "parallel.hpp"

#include <algorithm>
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

struct FoldRules {
    bool post_abs_real = false;
    bool abs_x = false;
    bool abs_axis = false;
    bool neg_imag = false;
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

inline __m256 avx2_abs_ps(__m256 v) {
    const __m256 sign = _mm256_set1_ps(-0.0f);
    return _mm256_andnot_ps(sign, v);
}

inline __m256 avx2_lane_mask_ps(int mask) {
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

inline __m256 real_projection(__m256 x2, __m256 axis2, const FoldRules& r) {
    __m256 q = _mm256_sub_ps(x2, axis2);
    return r.post_abs_real ? avx2_abs_ps(q) : q;
}

inline __m256 imag_projection(__m256 x, __m256 axis, const FoldRules& r) {
    const __m256 a = r.abs_x ? avx2_abs_ps(x) : x;
    const __m256 b = r.abs_axis ? avx2_abs_ps(axis) : axis;
    __m256 q = _mm256_add_ps(_mm256_mul_ps(a, b), _mm256_mul_ps(a, b));
    return r.neg_imag ? _mm256_sub_ps(_mm256_setzero_ps(), q) : q;
}

constexpr int MAX_TRANSITION_LEGS = 4;

struct ActiveVolumeLeg {
    FoldRules rules;
    float y_factor = 1.0f;
    float z_factor = 0.0f;
    float influence = 1.0f;
};

bool active_volume_legs_avx2(const std::vector<TransitionLeg>& input, ActiveVolumeLeg (&out)[MAX_TRANSITION_LEGS], int& count) {
    if (input.size() > MAX_TRANSITION_LEGS) return false;
    TransitionLeg kept[MAX_TRANSITION_LEGS];
    int kept_count = 0;
    double max_w = 0.0;
    double sum_w2 = 0.0;
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

    const int n = kept_count;
    const double rms_w = std::sqrt(sum_w2 / static_cast<double>(n));
    constexpr double PI = 3.14159265358979323846264338327950288;
    count = n;
    for (int i = 0; i < n; ++i) {
        double by = 1.0;
        double bz = 0.0;
        if (n == 2) {
            by = (i == 0) ? 1.0 : 0.0;
            bz = (i == 0) ? 0.0 : 1.0;
        } else if (n > 2) {
            const double angle = 2.0 * PI * static_cast<double>(i) / static_cast<double>(n);
            const double scale = std::sqrt(2.0 / static_cast<double>(n));
            by = std::cos(angle) * scale;
            bz = std::sin(angle) * scale;
        }
        const double axis_scale = kept[i].weight / rms_w;
        out[i] = {
            fold_rules(kept[i].variant),
            static_cast<float>(by * axis_scale),
            static_cast<float>(bz * axis_scale),
            static_cast<float>(kept[i].weight / max_w),
        };
    }
    return true;
}

bool build_range_impl(const TransitionVolumeParams& p, int N, int z_begin, int z_end, McField& field, bool threaded) {
    if (!avx2_available() || !fma_available()) return false;
    if (p.multi_legs.empty() && (!variant_supports_axis_transition(p.from_variant) ||
        !variant_supports_axis_transition(p.to_variant))) {
        return false;
    }
    if (N <= 0 || z_begin < 0 || z_end < z_begin || z_end > N) return false;
    if (field.data.size() < static_cast<size_t>(N) * N * N) return false;

    const float span = static_cast<float>(p.extent * 2.0);
    const float xmin = static_cast<float>(p.centerX - p.extent);
    const float ymin = static_cast<float>(p.centerY - p.extent);
    const float zmin = static_cast<float>(p.centerZ - p.extent);
    const float bail2 = static_cast<float>(p.bailout_sq);
    const float bailout = static_cast<float>(p.bailout);
    const int maxIter = p.iterations;

    const __m256 lane_offsets = _mm256_set_ps(7.5f, 6.5f, 5.5f, 4.5f, 3.5f, 2.5f, 1.5f, 0.5f);
    const __m256 vN = _mm256_set1_ps(static_cast<float>(N));
    const __m256 vspan = _mm256_set1_ps(span);
    const __m256 vxmin = _mm256_set1_ps(xmin);
    const __m256 vbail2 = _mm256_set1_ps(bail2);

    if (!p.multi_legs.empty()) {
        ActiveVolumeLeg legs[MAX_TRANSITION_LEGS];
        int leg_count = 0;
        if (!active_volume_legs_avx2(p.multi_legs, legs, leg_count)) return false;

        auto render_z_multi = [&](int zi) {
            const float z0s = zmin + (static_cast<float>(zi) + 0.5f) / static_cast<float>(N) * span;
            for (int yi = 0; yi < N; ++yi) {
                const float y0s = ymin + (static_cast<float>(yi) + 0.5f) / static_cast<float>(N) * span;
                for (int xi = 0; xi < N; xi += 8) {
                    int lane_mask = 0;
                    for (int k = 0; k < 8; ++k) {
                        if (xi + k < N) lane_mask |= (1 << k);
                    }
                    const __m256 vxi = _mm256_add_ps(_mm256_set1_ps(static_cast<float>(xi)), lane_offsets);
                    const __m256 vx0 = _mm256_fmadd_ps(_mm256_div_ps(vxi, vN), vspan, vxmin);

                    __m256 vx = vx0;
                    __m256 vx2 = _mm256_mul_ps(vx, vx);
                    __m256 vaxis[MAX_TRANSITION_LEGS];
                    __m256 vaxis2[MAX_TRANSITION_LEGS];
                    __m256 vcaxis[MAX_TRANSITION_LEGS];
                    for (int i = 0; i < leg_count; ++i) {
                        const float caxis = y0s * legs[i].y_factor + z0s * legs[i].z_factor;
                        vcaxis[i] = _mm256_set1_ps(caxis);
                        vaxis[i] = vcaxis[i];
                        vaxis2[i] = _mm256_mul_ps(vaxis[i], vaxis[i]);
                    }

                    int active = lane_mask;
                    int iters[8] = {
                        maxIter, maxIter, maxIter, maxIter,
                        maxIter, maxIter, maxIter, maxIter
                    };

                    for (int iter = 0; iter < maxIter && active; ++iter) {
                        __m256 real_sum = _mm256_setzero_ps();
                        float influence_sum = 0.0f;
                        __m256 next_axis[MAX_TRANSITION_LEGS];
                        for (int i = 0; i < leg_count; ++i) {
                            const __m256 inf = _mm256_set1_ps(legs[i].influence);
                            real_sum = _mm256_add_ps(real_sum,
                                _mm256_mul_ps(inf, real_projection(vx2, vaxis2[i], legs[i].rules)));
                            influence_sum += legs[i].influence;
                            next_axis[i] = _mm256_add_ps(
                                _mm256_mul_ps(inf, imag_projection(vx, vaxis[i], legs[i].rules)),
                                vcaxis[i]);
                        }

                        const __m256 nx = _mm256_add_ps(
                            _mm256_sub_ps(real_sum, _mm256_mul_ps(_mm256_set1_ps(influence_sum - 1.0f), vx2)),
                            vx0);
                        const __m256 nx2 = _mm256_mul_ps(nx, nx);
                        __m256 n2 = nx2;
                        for (int i = 0; i < leg_count; ++i) {
                            n2 = _mm256_add_ps(n2, _mm256_mul_ps(next_axis[i], next_axis[i]));
                        }

                        const __m256 escaped_radius = _mm256_cmp_ps(n2, vbail2, _CMP_GT_OQ);
                        const __m256 escaped_nan = _mm256_cmp_ps(n2, n2, _CMP_UNORD_Q);
                        const int escaped = _mm256_movemask_ps(_mm256_or_ps(escaped_radius, escaped_nan)) & active;
                        if (escaped) {
                            for (int k = 0; k < 8; ++k) {
                                if (escaped & (1 << k)) iters[k] = iter;
                            }
                            active &= ~escaped;
                        }

                        const __m256 active_vec = avx2_lane_mask_ps(active);
                        vx = _mm256_blendv_ps(vx, nx, active_vec);
                        vx2 = _mm256_blendv_ps(vx2, nx2, active_vec);
                        for (int i = 0; i < leg_count; ++i) {
                            const __m256 na2 = _mm256_mul_ps(next_axis[i], next_axis[i]);
                            vaxis[i] = _mm256_blendv_ps(vaxis[i], next_axis[i], active_vec);
                            vaxis2[i] = _mm256_blendv_ps(vaxis2[i], na2, active_vec);
                        }
                    }

                    alignas(32) float xs2[8];
                    alignas(32) float as2[MAX_TRANSITION_LEGS][8];
                    _mm256_store_ps(xs2, vx2);
                    for (int i = 0; i < leg_count; ++i) {
                        _mm256_store_ps(as2[i], vaxis2[i]);
                    }
                    for (int k = 0; k < 8 && xi + k < N; ++k) {
                        float value = 0.0f;
                        if (iters[k] < maxIter) {
                            value = 0.5f + 0.5f * (static_cast<float>(iters[k]) / static_cast<float>(maxIter));
                        } else {
                            float mag2 = xs2[k];
                            for (int i = 0; i < leg_count; ++i) mag2 += as2[i][k];
                            const float finalMag = std::isfinite(mag2) ? std::sqrt(mag2) : bailout;
                            value = std::min(0.48f, (finalMag / bailout) * 0.48f);
                        }
                        const size_t idx = static_cast<size_t>(xi + k) +
                            static_cast<size_t>(N) *
                            (static_cast<size_t>(yi) + static_cast<size_t>(N) * static_cast<size_t>(zi));
                        field.data[idx] = value;
                    }
                }
            }
        };

        if (threaded) {
            const int thread_count = default_render_threads();
            #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 1)
            for (int zi = z_begin; zi < z_end; ++zi) {
                render_z_multi(zi);
            }
        } else {
            for (int zi = z_begin; zi < z_end; ++zi) {
                render_z_multi(zi);
            }
        }

        return true;
    }

    const FoldRules from = fold_rules(p.from_variant);
    const FoldRules to = fold_rules(p.to_variant);

    auto render_z = [&](int zi) {
        const float z0s = zmin + (static_cast<float>(zi) + 0.5f) / static_cast<float>(N) * span;
        const __m256 vz0 = _mm256_set1_ps(z0s);
        for (int yi = 0; yi < N; ++yi) {
            const float y0s = ymin + (static_cast<float>(yi) + 0.5f) / static_cast<float>(N) * span;
            const __m256 vy0 = _mm256_set1_ps(y0s);
            for (int xi = 0; xi < N; xi += 8) {
                int lane_mask = 0;
                for (int k = 0; k < 8; ++k) {
                    if (xi + k < N) lane_mask |= (1 << k);
                }
                const __m256 vxi = _mm256_add_ps(_mm256_set1_ps(static_cast<float>(xi)), lane_offsets);
                const __m256 vx0 = _mm256_fmadd_ps(_mm256_div_ps(vxi, vN), vspan, vxmin);

                __m256 vx = vx0;
                __m256 vy = vy0;
                __m256 vz = vz0;
                __m256 vx2 = _mm256_mul_ps(vx, vx);
                __m256 vy2 = _mm256_mul_ps(vy, vy);
                __m256 vz2 = _mm256_mul_ps(vz, vz);
                int active = lane_mask;
                int iters[8] = {
                    maxIter, maxIter, maxIter, maxIter,
                    maxIter, maxIter, maxIter, maxIter
                };

                for (int iter = 0; iter < maxIter && active; ++iter) {
                    const __m256 nx = _mm256_add_ps(
                        _mm256_sub_ps(
                            _mm256_add_ps(real_projection(vx2, vy2, from),
                                          real_projection(vx2, vz2, to)),
                            vx2),
                        vx0);
                    const __m256 ny = _mm256_add_ps(imag_projection(vx, vy, from), vy0);
                    const __m256 nz = _mm256_add_ps(imag_projection(vx, vz, to), vz0);

                    const __m256 nx2 = _mm256_mul_ps(nx, nx);
                    const __m256 ny2 = _mm256_mul_ps(ny, ny);
                    const __m256 nz2 = _mm256_mul_ps(nz, nz);
                    const __m256 n2 = _mm256_add_ps(nx2, _mm256_add_ps(ny2, nz2));
                    const __m256 escaped_radius = _mm256_cmp_ps(n2, vbail2, _CMP_GT_OQ);
                    const __m256 escaped_nan = _mm256_cmp_ps(n2, n2, _CMP_UNORD_Q);
                    const int escaped = _mm256_movemask_ps(_mm256_or_ps(escaped_radius, escaped_nan)) & active;
                    if (escaped) {
                        for (int k = 0; k < 8; ++k) {
                            if (escaped & (1 << k)) iters[k] = iter;
                        }
                        active &= ~escaped;
                    }

                    const __m256 active_vec = avx2_lane_mask_ps(active);
                    vx = _mm256_blendv_ps(vx, nx, active_vec);
                    vy = _mm256_blendv_ps(vy, ny, active_vec);
                    vz = _mm256_blendv_ps(vz, nz, active_vec);
                    vx2 = _mm256_blendv_ps(vx2, nx2, active_vec);
                    vy2 = _mm256_blendv_ps(vy2, ny2, active_vec);
                    vz2 = _mm256_blendv_ps(vz2, nz2, active_vec);
                }

                alignas(32) float xs2[8], ys2[8], zs2[8];
                _mm256_store_ps(xs2, vx2);
                _mm256_store_ps(ys2, vy2);
                _mm256_store_ps(zs2, vz2);
                for (int k = 0; k < 8 && xi + k < N; ++k) {
                    float value = 0.0f;
                    if (iters[k] < maxIter) {
                        value = 0.5f + 0.5f * (static_cast<float>(iters[k]) / static_cast<float>(maxIter));
                    } else {
                        const float mag2 = xs2[k] + ys2[k] + zs2[k];
                        const float finalMag = std::isfinite(mag2) ? std::sqrt(mag2) : bailout;
                        value = std::min(0.48f, (finalMag / bailout) * 0.48f);
                    }
                    const size_t idx = static_cast<size_t>(xi + k) +
                        static_cast<size_t>(N) *
                        (static_cast<size_t>(yi) + static_cast<size_t>(N) * static_cast<size_t>(zi));
                    field.data[idx] = value;
                }
            }
        }
    };

    if (threaded) {
        const int thread_count = default_render_threads();
        #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 1)
        for (int zi = z_begin; zi < z_end; ++zi) {
            render_z(zi);
        }
    } else {
        for (int zi = z_begin; zi < z_end; ++zi) {
            render_z(zi);
        }
    }

    return true;
}

} // namespace

bool buildTransitionVolumeAvx2Range(const TransitionVolumeParams& p, int N, int z_begin, int z_end, McField& field) {
    return build_range_impl(p, N, z_begin, z_end, field, false);
}

bool buildTransitionVolumeAvx2(const TransitionVolumeParams& p, McField& field) {
    if (!avx2_available() || !fma_available()) return false;
    if (p.multi_legs.empty() && (!variant_supports_axis_transition(p.from_variant) ||
        !variant_supports_axis_transition(p.to_variant))) {
        return false;
    }

    const int N = std::max(4, std::min(1024, p.resolution));
    field.Nx = field.Ny = field.Nz = N;
    field.data.assign(static_cast<size_t>(N) * N * N, 1.0f);
    field.scalar_used = "fp32";
    field.engine_used = p.multi_legs.empty() ? "avx2_fp32" : "avx2_multi_fp32";
    return build_range_impl(p, N, 0, N, field, true);
}

#else

bool buildTransitionVolumeAvx2Range(const TransitionVolumeParams& p, int N, int z_begin, int z_end, McField& field) {
    (void)p; (void)N; (void)z_begin; (void)z_end; (void)field;
    return false;
}

bool buildTransitionVolumeAvx2(const TransitionVolumeParams& p, McField& field) {
    (void)p; (void)field;
    return false;
}

#endif

} // namespace fsd::compute
