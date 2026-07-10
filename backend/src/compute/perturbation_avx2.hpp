// compute/perturbation_avx2.hpp
//
// AVX2+FMA batch kernel for the perturbation delta iteration (see
// perturbation.hpp for the algorithm). Four pixels per vector; each lane
// keeps its own reference index and rebase state, so reference lookups use
// per-lane gathers on a combined orbit table:
//
//   tab[0 .. )              primary orbit R (start_off/start_len select it)
//   tab[k_off .. k_off+k_len)  rebase orbit K whose Z_0 = 0
//
// Mandelbrot passes k_off == start_off == 0 (K is R). Julia appends the
// critical orbit after the seeded orbit and passes k_off = len(R).

#pragma once

#include <cstdint>

namespace fsd::compute {

// Compile-time + runtime availability (AVX2 and FMA).
bool perturb_avx2_available() noexcept;

// Iterate `count` pixels. dz0/dc are per-pixel initial delta and c-offset
// (Julia: dz0 = pixel offset, dc = 0; Mandelbrot: dz0 = 0, dc = offset).
// Outputs per pixel: escape iteration (max_iter when the pixel never
// escapes) and |z|^2 at escape (0 when it never escapes).
// Requires start_len >= 2 and k_len >= 2.
void perturb_iterate_batch_avx2(
    const double* tab_re, const double* tab_im,
    int start_off, int start_len,
    int k_off, int k_len,
    const double* dz0_re, const double* dz0_im,
    const double* dc_re, const double* dc_im,
    int count, int max_iter, double bail2,
    int32_t* out_iter, double* out_mag2) noexcept;

// fp32-delta variant: eight lanes per vector against a float reference
// table (the driver downconverts the double orbit once per render).
void perturb_iterate_batch_avx2_fp32(
    const float* tab_re, const float* tab_im,
    int start_off, int start_len,
    int k_off, int k_len,
    const float* dz0_re, const float* dz0_im,
    const float* dc_re, const float* dc_im,
    int count, int max_iter, float bail2,
    int32_t* out_iter, float* out_mag2) noexcept;

} // namespace fsd::compute
