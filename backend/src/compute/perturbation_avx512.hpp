// compute/perturbation_avx512.hpp
//
// AVX-512 batch kernel for the perturbation delta iteration — 8 pixels per
// vector, same contract as perturb_iterate_batch_avx2 (see
// perturbation_avx2.hpp for the combined-orbit-table layout).

#pragma once

#include <cstdint>

namespace fsd::compute {

// Compile-time + runtime availability (AVX-512F with OS state).
bool perturb_avx512_available() noexcept;

void perturb_iterate_batch_avx512(
    const double* tab_re, const double* tab_im,
    int start_off, int start_len,
    int k_off, int k_len,
    const double* dz0_re, const double* dz0_im,
    const double* dc_re, const double* dc_im,
    int count, int max_iter, double bail2,
    int32_t* out_iter, double* out_mag2) noexcept;

// fp32-delta variant: sixteen lanes per vector against a float reference
// table (the driver downconverts the double orbit once per render).
void perturb_iterate_batch_avx512_fp32(
    const float* tab_re, const float* tab_im,
    int start_off, int start_len,
    int k_off, int k_len,
    const float* dz0_re, const float* dz0_im,
    const float* dc_re, const float* dc_im,
    int count, int max_iter, float bail2,
    int32_t* out_iter, float* out_mag2) noexcept;

} // namespace fsd::compute
