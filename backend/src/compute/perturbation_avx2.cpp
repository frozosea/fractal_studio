// compute/perturbation_avx2.cpp
//
// AVX2+FMA implementation of the batched perturbation delta iteration.
// Mirrors perturb_iterate() in perturbation.hpp lane-for-lane: per-lane
// reference index m, per-lane orbit window (base offset + length), Zhuoran
// rebasing onto the K orbit. Escaped lanes freeze their state so the shared
// gathers stay in bounds until the whole group finishes.

#include "perturbation_avx2.hpp"

#include "cpu_features.hpp"

#if defined(__AVX2__) && defined(__FMA__)
#  include <immintrin.h>
#endif

namespace fsd::compute {

#if defined(__AVX2__) && defined(__FMA__)

bool perturb_avx2_available() noexcept {
    return avx2_available() && fma_available();
}

namespace {

inline __m256i blend_epi64(__m256i a, __m256i b, __m256d mask) {
    return _mm256_castpd_si256(_mm256_blendv_pd(
        _mm256_castsi256_pd(a), _mm256_castsi256_pd(b), mask));
}

} // namespace

void perturb_iterate_batch_avx2(
    const double* tab_re, const double* tab_im,
    int start_off, int start_len,
    int k_off, int k_len,
    const double* dz0_re, const double* dz0_im,
    const double* dc_re, const double* dc_im,
    int count, int max_iter, double bail2,
    int32_t* out_iter, double* out_mag2) noexcept
{
    const __m256d vbail2 = _mm256_set1_pd(bail2);
    const __m256d vtwo   = _mm256_set1_pd(2.0);
    const __m256i vone   = _mm256_set1_epi64x(1);
    const __m256i vzero  = _mm256_setzero_si256();
    const __m256i vkoff  = _mm256_set1_epi64x(k_off);
    const __m256i vklen1 = _mm256_set1_epi64x(static_cast<int64_t>(k_len) - 1);

    for (int i = 0; i < count; i += 4) {
        const int lanes = (count - i) < 4 ? (count - i) : 4;

        alignas(32) double b_dzr[4] = {0, 0, 0, 0};
        alignas(32) double b_dzi[4] = {0, 0, 0, 0};
        alignas(32) double b_dcr[4] = {0, 0, 0, 0};
        alignas(32) double b_dci[4] = {0, 0, 0, 0};
        for (int l = 0; l < lanes; ++l) {
            b_dzr[l] = dz0_re[i + l];
            b_dzi[l] = dz0_im[i + l];
            b_dcr[l] = dc_re[i + l];
            b_dci[l] = dc_im[i + l];
        }
        __m256d dzr = _mm256_load_pd(b_dzr);
        __m256d dzi = _mm256_load_pd(b_dzi);
        const __m256d dcr = _mm256_load_pd(b_dcr);
        const __m256d dci = _mm256_load_pd(b_dci);

        __m256i base   = _mm256_set1_epi64x(start_off);
        __m256i olenm1 = _mm256_set1_epi64x(static_cast<int64_t>(start_len) - 1);
        __m256i m      = vzero;
        __m256i iter   = _mm256_set1_epi64x(max_iter);
        __m256d mag2out = _mm256_setzero_pd();

        // Tail lanes start inactive: state frozen at m = 0 (always in bounds).
        alignas(32) int64_t amask[4];
        for (int l = 0; l < 4; ++l) amask[l] = l < lanes ? -1LL : 0LL;
        __m256d active = _mm256_castsi256_pd(
            _mm256_load_si256(reinterpret_cast<const __m256i*>(amask)));

        for (int n = 0; n < max_iter; ++n) {
            const __m256i idx  = _mm256_add_epi64(base, m);
            const __m256i idx1 = _mm256_add_epi64(idx, vone);
            const __m256d Zr  = _mm256_i64gather_pd(tab_re, idx, 8);
            const __m256d Zi  = _mm256_i64gather_pd(tab_im, idx, 8);
            const __m256d Zr1 = _mm256_i64gather_pd(tab_re, idx1, 8);
            const __m256d Zi1 = _mm256_i64gather_pd(tab_im, idx1, 8);

            // dz' = 2*Z*dz + dz^2 + dc
            const __m256d tZr = _mm256_mul_pd(vtwo, Zr);
            const __m256d tZi = _mm256_mul_pd(vtwo, Zi);
            __m256d ndr = _mm256_fmadd_pd(tZr, dzr, dcr);
            ndr = _mm256_fnmadd_pd(tZi, dzi, ndr);
            ndr = _mm256_fmadd_pd(dzr, dzr, ndr);
            ndr = _mm256_fnmadd_pd(dzi, dzi, ndr);
            __m256d ndi = _mm256_fmadd_pd(tZr, dzi, dci);
            ndi = _mm256_fmadd_pd(tZi, dzr, ndi);
            ndi = _mm256_fmadd_pd(_mm256_mul_pd(vtwo, dzr), dzi, ndi);

            dzr = _mm256_blendv_pd(dzr, ndr, active);
            dzi = _mm256_blendv_pd(dzi, ndi, active);

            const __m256d zr = _mm256_add_pd(Zr1, dzr);
            const __m256d zi = _mm256_add_pd(Zi1, dzi);
            const __m256d mag2 = _mm256_fmadd_pd(zr, zr, _mm256_mul_pd(zi, zi));

            const __m256d esc = _mm256_and_pd(
                _mm256_cmp_pd(mag2, vbail2, _CMP_GT_OQ), active);
            iter = blend_epi64(iter, _mm256_set1_epi64x(n), esc);
            mag2out = _mm256_blendv_pd(mag2out, mag2, esc);
            active = _mm256_andnot_pd(esc, active);
            if (_mm256_movemask_pd(active) == 0) break;

            const __m256i m1 = _mm256_add_epi64(m, vone);

            // Rebase when |z|^2 < |dz|^2 or the orbit window ends (m1+1 > olen-1).
            const __m256d dz2 = _mm256_fmadd_pd(dzr, dzr, _mm256_mul_pd(dzi, dzi));
            const __m256d cancel = _mm256_cmp_pd(mag2, dz2, _CMP_LT_OQ);
            const __m256d exhaust = _mm256_castsi256_pd(
                _mm256_cmpgt_epi64(_mm256_add_epi64(m1, vone), olenm1));
            const __m256d rebase = _mm256_and_pd(_mm256_or_pd(cancel, exhaust), active);

            dzr = _mm256_blendv_pd(dzr, zr, rebase);
            dzi = _mm256_blendv_pd(dzi, zi, rebase);
            m = blend_epi64(m, blend_epi64(m1, vzero, rebase), active);
            base = blend_epi64(base, vkoff, rebase);
            olenm1 = blend_epi64(olenm1, vklen1, rebase);
        }

        alignas(32) int64_t it64[4];
        alignas(32) double mg[4];
        _mm256_store_si256(reinterpret_cast<__m256i*>(it64), iter);
        _mm256_store_pd(mg, mag2out);
        for (int l = 0; l < lanes; ++l) {
            out_iter[i + l] = static_cast<int32_t>(it64[l]);
            out_mag2[i + l] = mg[l];
        }
    }
}

// fp32 deltas: eight lanes per vector, 32-bit lane indices, same rebasing
// state machine as the fp64 kernel above. Lane masks live in float vectors
// (sign-bit convention, like the fp64 kernel's double masks).
void perturb_iterate_batch_avx2_fp32(
    const float* tab_re, const float* tab_im,
    int start_off, int start_len,
    int k_off, int k_len,
    const float* dz0_re, const float* dz0_im,
    const float* dc_re, const float* dc_im,
    int count, int max_iter, float bail2,
    int32_t* out_iter, float* out_mag2) noexcept
{
    const __m256 vbail2 = _mm256_set1_ps(bail2);
    const __m256 vtwo   = _mm256_set1_ps(2.0f);
    const __m256i vone  = _mm256_set1_epi32(1);
    const __m256i vzero = _mm256_setzero_si256();
    const __m256i vkoff  = _mm256_set1_epi32(k_off);
    const __m256i vklen1 = _mm256_set1_epi32(k_len - 1);

    auto blend_epi32f = [](__m256i a, __m256i b, __m256 mask) {
        return _mm256_castps_si256(_mm256_blendv_ps(
            _mm256_castsi256_ps(a), _mm256_castsi256_ps(b), mask));
    };

    for (int i = 0; i < count; i += 8) {
        const int lanes = (count - i) < 8 ? (count - i) : 8;

        alignas(32) float b_dzr[8] = {0};
        alignas(32) float b_dzi[8] = {0};
        alignas(32) float b_dcr[8] = {0};
        alignas(32) float b_dci[8] = {0};
        for (int l = 0; l < lanes; ++l) {
            b_dzr[l] = dz0_re[i + l];
            b_dzi[l] = dz0_im[i + l];
            b_dcr[l] = dc_re[i + l];
            b_dci[l] = dc_im[i + l];
        }
        __m256 dzr = _mm256_load_ps(b_dzr);
        __m256 dzi = _mm256_load_ps(b_dzi);
        const __m256 dcr = _mm256_load_ps(b_dcr);
        const __m256 dci = _mm256_load_ps(b_dci);

        __m256i base   = _mm256_set1_epi32(start_off);
        __m256i olenm1 = _mm256_set1_epi32(start_len - 1);
        __m256i m      = vzero;
        __m256i iter   = _mm256_set1_epi32(max_iter);
        __m256 mag2out = _mm256_setzero_ps();

        // Tail lanes start inactive: state frozen at m = 0 (always in bounds).
        alignas(32) int32_t amask[8];
        for (int l = 0; l < 8; ++l) amask[l] = l < lanes ? -1 : 0;
        __m256 active = _mm256_castsi256_ps(
            _mm256_load_si256(reinterpret_cast<const __m256i*>(amask)));

        for (int n = 0; n < max_iter; ++n) {
            const __m256i idx  = _mm256_add_epi32(base, m);
            const __m256i idx1 = _mm256_add_epi32(idx, vone);
            const __m256 Zr  = _mm256_i32gather_ps(tab_re, idx, 4);
            const __m256 Zi  = _mm256_i32gather_ps(tab_im, idx, 4);
            const __m256 Zr1 = _mm256_i32gather_ps(tab_re, idx1, 4);
            const __m256 Zi1 = _mm256_i32gather_ps(tab_im, idx1, 4);

            // dz' = 2*Z*dz + dz^2 + dc
            const __m256 tZr = _mm256_mul_ps(vtwo, Zr);
            const __m256 tZi = _mm256_mul_ps(vtwo, Zi);
            __m256 ndr = _mm256_fmadd_ps(tZr, dzr, dcr);
            ndr = _mm256_fnmadd_ps(tZi, dzi, ndr);
            ndr = _mm256_fmadd_ps(dzr, dzr, ndr);
            ndr = _mm256_fnmadd_ps(dzi, dzi, ndr);
            __m256 ndi = _mm256_fmadd_ps(tZr, dzi, dci);
            ndi = _mm256_fmadd_ps(tZi, dzr, ndi);
            ndi = _mm256_fmadd_ps(_mm256_mul_ps(vtwo, dzr), dzi, ndi);

            dzr = _mm256_blendv_ps(dzr, ndr, active);
            dzi = _mm256_blendv_ps(dzi, ndi, active);

            const __m256 zr = _mm256_add_ps(Zr1, dzr);
            const __m256 zi = _mm256_add_ps(Zi1, dzi);
            const __m256 mag2 = _mm256_fmadd_ps(zr, zr, _mm256_mul_ps(zi, zi));

            const __m256 esc = _mm256_and_ps(
                _mm256_cmp_ps(mag2, vbail2, _CMP_GT_OQ), active);
            iter = blend_epi32f(iter, _mm256_set1_epi32(n), esc);
            mag2out = _mm256_blendv_ps(mag2out, mag2, esc);
            active = _mm256_andnot_ps(esc, active);
            if (_mm256_movemask_ps(active) == 0) break;

            const __m256i m1 = _mm256_add_epi32(m, vone);

            // Rebase when |z|^2 < |dz|^2 or the orbit window ends (m1+1 > olen-1).
            const __m256 dz2 = _mm256_fmadd_ps(dzr, dzr, _mm256_mul_ps(dzi, dzi));
            const __m256 cancel = _mm256_cmp_ps(mag2, dz2, _CMP_LT_OQ);
            const __m256 exhaust = _mm256_castsi256_ps(
                _mm256_cmpgt_epi32(_mm256_add_epi32(m1, vone), olenm1));
            const __m256 rebase = _mm256_and_ps(_mm256_or_ps(cancel, exhaust), active);

            dzr = _mm256_blendv_ps(dzr, zr, rebase);
            dzi = _mm256_blendv_ps(dzi, zi, rebase);
            m = blend_epi32f(m, blend_epi32f(m1, vzero, rebase), active);
            base = blend_epi32f(base, vkoff, rebase);
            olenm1 = blend_epi32f(olenm1, vklen1, rebase);
        }

        alignas(32) int32_t it32[8];
        alignas(32) float mg[8];
        _mm256_store_si256(reinterpret_cast<__m256i*>(it32), iter);
        _mm256_store_ps(mg, mag2out);
        for (int l = 0; l < lanes; ++l) {
            out_iter[i + l] = it32[l];
            out_mag2[i + l] = mg[l];
        }
    }
}

#else // !(__AVX2__ && __FMA__)

bool perturb_avx2_available() noexcept { return false; }

void perturb_iterate_batch_avx2(
    const double*, const double*, int, int, int, int,
    const double*, const double*, const double*, const double*,
    int, int, double, int32_t*, double*) noexcept
{
    // Unreachable: callers gate on perturb_avx2_available().
}

void perturb_iterate_batch_avx2_fp32(
    const float*, const float*, int, int, int, int,
    const float*, const float*, const float*, const float*,
    int, int, float, int32_t*, float*) noexcept
{
    // Unreachable: callers gate on perturb_avx2_available().
}

#endif

} // namespace fsd::compute
