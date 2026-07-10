// compute/cuda/perturb_kernel.cuh
//
// Host-side interface to the CUDA perturbation renderer (deep-zoom
// Mandelbrot/Julia, fp64 or fp32 deltas with Zhuoran rebasing — mirrors
// fsd::compute::perturb_iterate).
//
// The reference data is one combined table: the primary orbit R selected by
// [start_off, start_off+start_len) and the rebase orbit K (Z_0 = 0) at
// [k_off, k_off+k_len). Mandelbrot passes start_off == k_off == 0 (K is R);
// Julia appends the critical orbit after the seeded orbit.

#pragma once

#include <cstdint>

namespace fsd_cuda {

struct CudaPerturbParams {
    int width = 0;
    int height = 0;
    int iterations = 0;
    double bailout_sq = 4.0;

    // Pixel offset generation.
    //   mode 0 (cartesian map): off = rot(span * frac(x, y))
    //   mode 1 (log-polar strip): off = exp(ln_r0 - y*k_step) * (cos, sin)(x*theta_step)
    int offset_mode = 0;
    double span_re = 0.0, span_im = 0.0;   // mode 0
    double cos_t = 1.0, sin_t = 0.0;       // mode 0 rotation
    double ln_r0 = 0.0;                    // mode 1
    double k_step = 0.0;                   // mode 1
    double theta_step = 0.0;               // mode 1

    // Julia: pixel offset enters as delta_z0 (dc = 0); else as delta_c.
    bool julia = false;

    // Iterate deltas in fp32 instead of fp64. Consumer GPUs (RTX 40 series:
    // 1:64 fp64:fp32 throughput) run this path dramatically faster. The
    // reference table is downconverted to float on the host; pixel offsets
    // are still generated in fp64 (once per pixel) before narrowing. Only
    // sensible while |dz| stays clear of the fp32 denormal floor — callers
    // gate by scale (good to ~1e-30).
    bool fp32_delta = false;

    // Added to every pixel's initial delta_z. Non-zero only when the primary
    // orbit is degenerate (seed escaped instantly): pixels then start on K
    // with dz shifted by R_0.
    double dz_shift_re = 0.0, dz_shift_im = 0.0;

    // Combined reference table (host memory; uploaded per call).
    const double* tab_re = nullptr;
    const double* tab_im = nullptr;
    int tab_len = 0;
    int start_off = 0, start_len = 0;      // start_len >= 2
    int k_off = 0, k_len = 0;              // k_len >= 2
};

// Renders the escape field: per-pixel iteration counts (== iterations when
// the pixel never escapes) and |z|^2 at escape (0 when it never escapes)
// into caller-provided host buffers of width*height entries.
// Returns false on any CUDA error (caller falls back to a CPU path).
bool cuda_render_perturb_field(
    const CudaPerturbParams& p,
    uint32_t* iter_u32, float* norm_f32,
    double* elapsed_ms);

} // namespace fsd_cuda
