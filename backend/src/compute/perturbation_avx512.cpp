// compute/perturbation_avx512.cpp
//
// AVX-512 implementation of the batched perturbation delta iteration.
// Eight lanes per vector; per-lane reference index and rebase window with
// mask registers for lane state. Mirrors perturb_iterate() exactly.

#include "perturbation_avx512.hpp"

#include "map_kernel_avx512.hpp"   // avx512_available(): CPUID + OS state

#if defined(__AVX512F__)
#  include <immintrin.h>
#endif

namespace fsd::compute {

#if defined(__AVX512F__)

bool perturb_avx512_available() noexcept {
    return avx512_available();
}

void perturb_iterate_batch_avx512(
    const double* tab_re, const double* tab_im,
    int start_off, int start_len,
    int k_off, int k_len,
    const double* dz0_re, const double* dz0_im,
    const double* dc_re, const double* dc_im,
    int count, int max_iter, double bail2,
    int32_t* out_iter, double* out_mag2) noexcept
{
    const __m512d vbail2 = _mm512_set1_pd(bail2);
    const __m512d vtwo   = _mm512_set1_pd(2.0);
    const __m512i vone   = _mm512_set1_epi64(1);
    const __m512i vzero  = _mm512_setzero_si512();
    const __m512i vkoff  = _mm512_set1_epi64(k_off);
    const __m512i vklen1 = _mm512_set1_epi64(static_cast<int64_t>(k_len) - 1);

    for (int i = 0; i < count; i += 8) {
        const int lanes = (count - i) < 8 ? (count - i) : 8;

        alignas(64) double b_dzr[8] = {0};
        alignas(64) double b_dzi[8] = {0};
        alignas(64) double b_dcr[8] = {0};
        alignas(64) double b_dci[8] = {0};
        for (int l = 0; l < lanes; ++l) {
            b_dzr[l] = dz0_re[i + l];
            b_dzi[l] = dz0_im[i + l];
            b_dcr[l] = dc_re[i + l];
            b_dci[l] = dc_im[i + l];
        }
        __m512d dzr = _mm512_load_pd(b_dzr);
        __m512d dzi = _mm512_load_pd(b_dzi);
        const __m512d dcr = _mm512_load_pd(b_dcr);
        const __m512d dci = _mm512_load_pd(b_dci);

        __m512i base   = _mm512_set1_epi64(start_off);
        __m512i olenm1 = _mm512_set1_epi64(static_cast<int64_t>(start_len) - 1);
        __m512i m      = vzero;
        __m512i iter   = _mm512_set1_epi64(max_iter);
        __m512d mag2out = _mm512_setzero_pd();

        __mmask8 active = static_cast<__mmask8>((1u << lanes) - 1u);

        for (int n = 0; n < max_iter; ++n) {
            const __m512i idx  = _mm512_add_epi64(base, m);
            const __m512i idx1 = _mm512_add_epi64(idx, vone);
            const __m512d Zr  = _mm512_i64gather_pd(idx, tab_re, 8);
            const __m512d Zi  = _mm512_i64gather_pd(idx, tab_im, 8);
            const __m512d Zr1 = _mm512_i64gather_pd(idx1, tab_re, 8);
            const __m512d Zi1 = _mm512_i64gather_pd(idx1, tab_im, 8);

            // dz' = 2*Z*dz + dz^2 + dc
            const __m512d tZr = _mm512_mul_pd(vtwo, Zr);
            const __m512d tZi = _mm512_mul_pd(vtwo, Zi);
            __m512d ndr = _mm512_fmadd_pd(tZr, dzr, dcr);
            ndr = _mm512_fnmadd_pd(tZi, dzi, ndr);
            ndr = _mm512_fmadd_pd(dzr, dzr, ndr);
            ndr = _mm512_fnmadd_pd(dzi, dzi, ndr);
            __m512d ndi = _mm512_fmadd_pd(tZr, dzi, dci);
            ndi = _mm512_fmadd_pd(tZi, dzr, ndi);
            ndi = _mm512_fmadd_pd(_mm512_mul_pd(vtwo, dzr), dzi, ndi);

            dzr = _mm512_mask_blend_pd(active, dzr, ndr);
            dzi = _mm512_mask_blend_pd(active, dzi, ndi);

            const __m512d zr = _mm512_add_pd(Zr1, dzr);
            const __m512d zi = _mm512_add_pd(Zi1, dzi);
            const __m512d mag2 = _mm512_fmadd_pd(zr, zr, _mm512_mul_pd(zi, zi));

            const __mmask8 esc =
                _mm512_cmp_pd_mask(mag2, vbail2, _CMP_GT_OQ) & active;
            iter = _mm512_mask_blend_epi64(esc, iter, _mm512_set1_epi64(n));
            mag2out = _mm512_mask_blend_pd(esc, mag2out, mag2);
            active = static_cast<__mmask8>(active & ~esc);
            if (active == 0) break;

            const __m512i m1 = _mm512_add_epi64(m, vone);

            // Rebase when |z|^2 < |dz|^2 or the orbit window ends.
            const __m512d dz2 = _mm512_fmadd_pd(dzr, dzr, _mm512_mul_pd(dzi, dzi));
            const __mmask8 cancel = _mm512_cmp_pd_mask(mag2, dz2, _CMP_LT_OQ);
            const __mmask8 exhaust =
                _mm512_cmpgt_epi64_mask(_mm512_add_epi64(m1, vone), olenm1);
            const __mmask8 rebase = static_cast<__mmask8>((cancel | exhaust) & active);

            dzr = _mm512_mask_blend_pd(rebase, dzr, zr);
            dzi = _mm512_mask_blend_pd(rebase, dzi, zi);
            m = _mm512_mask_blend_epi64(active, m,
                    _mm512_mask_blend_epi64(rebase, m1, vzero));
            base = _mm512_mask_blend_epi64(rebase, base, vkoff);
            olenm1 = _mm512_mask_blend_epi64(rebase, olenm1, vklen1);
        }

        alignas(64) int64_t it64[8];
        alignas(64) double mg[8];
        _mm512_store_si512(reinterpret_cast<__m512i*>(it64), iter);
        _mm512_store_pd(mg, mag2out);
        for (int l = 0; l < lanes; ++l) {
            out_iter[i + l] = static_cast<int32_t>(it64[l]);
            out_mag2[i + l] = mg[l];
        }
    }
}

// fp32 deltas: sixteen lanes per vector, 32-bit lane indices, same rebasing
// state machine as the fp64 kernel above.
void perturb_iterate_batch_avx512_fp32(
    const float* tab_re, const float* tab_im,
    int start_off, int start_len,
    int k_off, int k_len,
    const float* dz0_re, const float* dz0_im,
    const float* dc_re, const float* dc_im,
    int count, int max_iter, float bail2,
    int32_t* out_iter, float* out_mag2) noexcept
{
    const __m512 vbail2 = _mm512_set1_ps(bail2);
    const __m512 vtwo   = _mm512_set1_ps(2.0f);
    const __m512i vone  = _mm512_set1_epi32(1);
    const __m512i vzero = _mm512_setzero_si512();
    const __m512i vkoff  = _mm512_set1_epi32(k_off);
    const __m512i vklen1 = _mm512_set1_epi32(k_len - 1);

    for (int i = 0; i < count; i += 16) {
        const int lanes = (count - i) < 16 ? (count - i) : 16;

        alignas(64) float b_dzr[16] = {0};
        alignas(64) float b_dzi[16] = {0};
        alignas(64) float b_dcr[16] = {0};
        alignas(64) float b_dci[16] = {0};
        for (int l = 0; l < lanes; ++l) {
            b_dzr[l] = dz0_re[i + l];
            b_dzi[l] = dz0_im[i + l];
            b_dcr[l] = dc_re[i + l];
            b_dci[l] = dc_im[i + l];
        }
        __m512 dzr = _mm512_load_ps(b_dzr);
        __m512 dzi = _mm512_load_ps(b_dzi);
        const __m512 dcr = _mm512_load_ps(b_dcr);
        const __m512 dci = _mm512_load_ps(b_dci);

        __m512i base   = _mm512_set1_epi32(start_off);
        __m512i olenm1 = _mm512_set1_epi32(start_len - 1);
        __m512i m      = vzero;
        __m512i iter   = _mm512_set1_epi32(max_iter);
        __m512 mag2out = _mm512_setzero_ps();

        __mmask16 active = static_cast<__mmask16>((1u << lanes) - 1u);

        for (int n = 0; n < max_iter; ++n) {
            const __m512i idx  = _mm512_add_epi32(base, m);
            const __m512i idx1 = _mm512_add_epi32(idx, vone);
            const __m512 Zr  = _mm512_i32gather_ps(idx, tab_re, 4);
            const __m512 Zi  = _mm512_i32gather_ps(idx, tab_im, 4);
            const __m512 Zr1 = _mm512_i32gather_ps(idx1, tab_re, 4);
            const __m512 Zi1 = _mm512_i32gather_ps(idx1, tab_im, 4);

            // dz' = 2*Z*dz + dz^2 + dc
            const __m512 tZr = _mm512_mul_ps(vtwo, Zr);
            const __m512 tZi = _mm512_mul_ps(vtwo, Zi);
            __m512 ndr = _mm512_fmadd_ps(tZr, dzr, dcr);
            ndr = _mm512_fnmadd_ps(tZi, dzi, ndr);
            ndr = _mm512_fmadd_ps(dzr, dzr, ndr);
            ndr = _mm512_fnmadd_ps(dzi, dzi, ndr);
            __m512 ndi = _mm512_fmadd_ps(tZr, dzi, dci);
            ndi = _mm512_fmadd_ps(tZi, dzr, ndi);
            ndi = _mm512_fmadd_ps(_mm512_mul_ps(vtwo, dzr), dzi, ndi);

            dzr = _mm512_mask_blend_ps(active, dzr, ndr);
            dzi = _mm512_mask_blend_ps(active, dzi, ndi);

            const __m512 zr = _mm512_add_ps(Zr1, dzr);
            const __m512 zi = _mm512_add_ps(Zi1, dzi);
            const __m512 mag2 = _mm512_fmadd_ps(zr, zr, _mm512_mul_ps(zi, zi));

            const __mmask16 esc =
                _mm512_cmp_ps_mask(mag2, vbail2, _CMP_GT_OQ) & active;
            iter = _mm512_mask_blend_epi32(esc, iter, _mm512_set1_epi32(n));
            mag2out = _mm512_mask_blend_ps(esc, mag2out, mag2);
            active = static_cast<__mmask16>(active & ~esc);
            if (active == 0) break;

            const __m512i m1 = _mm512_add_epi32(m, vone);

            // Rebase when |z|^2 < |dz|^2 or the orbit window ends.
            const __m512 dz2 = _mm512_fmadd_ps(dzr, dzr, _mm512_mul_ps(dzi, dzi));
            const __mmask16 cancel = _mm512_cmp_ps_mask(mag2, dz2, _CMP_LT_OQ);
            const __mmask16 exhaust =
                _mm512_cmpgt_epi32_mask(_mm512_add_epi32(m1, vone), olenm1);
            const __mmask16 rebase = static_cast<__mmask16>((cancel | exhaust) & active);

            dzr = _mm512_mask_blend_ps(rebase, dzr, zr);
            dzi = _mm512_mask_blend_ps(rebase, dzi, zi);
            m = _mm512_mask_blend_epi32(active, m,
                    _mm512_mask_blend_epi32(rebase, m1, vzero));
            base = _mm512_mask_blend_epi32(rebase, base, vkoff);
            olenm1 = _mm512_mask_blend_epi32(rebase, olenm1, vklen1);
        }

        alignas(64) int32_t it32[16];
        alignas(64) float mg[16];
        _mm512_store_si512(reinterpret_cast<__m512i*>(it32), iter);
        _mm512_store_ps(mg, mag2out);
        for (int l = 0; l < lanes; ++l) {
            out_iter[i + l] = it32[l];
            out_mag2[i + l] = mg[l];
        }
    }
}

#else // !__AVX512F__

bool perturb_avx512_available() noexcept { return false; }

void perturb_iterate_batch_avx512(
    const double*, const double*, int, int, int, int,
    const double*, const double*, const double*, const double*,
    int, int, double, int32_t*, double*) noexcept
{
    // Unreachable: callers gate on perturb_avx512_available().
}

void perturb_iterate_batch_avx512_fp32(
    const float*, const float*, int, int, int, int,
    const float*, const float*, const float*, const float*,
    int, int, float, int32_t*, float*) noexcept
{
    // Unreachable: callers gate on perturb_avx512_available().
}

#endif

} // namespace fsd::compute
