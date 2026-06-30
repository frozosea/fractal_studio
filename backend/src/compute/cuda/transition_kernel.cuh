// compute/cuda/transition_kernel.cuh
//
// CUDA 2D transition slice renderer — field output (escape iter+norm).

#pragma once

#include <cstdint>
#include <string>

namespace fsd_cuda {

constexpr int CUDA_MAX_TRANSITION_LEGS = 4;

struct CudaTransitionSliceParams {
    double center_re = -0.75;
    double center_im =  0.0;
    double scale     =  3.0;
    int    width     = 1024;
    int    height    = 768;
    int    iterations = 1024;
    double bailout_sq = 4.0;
    double cos_theta = 1.0;
    double sin_theta = 0.0;
    int    from_variant = 0;
    int    to_variant   = 2;
    int    multi_count  = 0;
    int    multi_variants[CUDA_MAX_TRANSITION_LEGS] = {0, 0, 0, 0};
    double multi_direction[CUDA_MAX_TRANSITION_LEGS] = {1.0, 0.0, 0.0, 0.0};
    double multi_influence[CUDA_MAX_TRANSITION_LEGS] = {1.0, 0.0, 0.0, 0.0};
    int    metric_id    = 0;   // 0=Escape, 1=MinAbs, 2=MaxAbs, 3=Envelope
    bool   julia     = false;
    double julia_re  = 0.0;
    double julia_im  = 0.0;
    std::string scalar_type = "fp64";
};

struct CudaTransitionSliceStats {
    double elapsed_ms = 0.0;
    std::string scalar_used;
    std::string engine_used = "cuda";
};

bool cuda_transition_slice_available() noexcept;

CudaTransitionSliceStats cuda_render_transition_slice_escape(
    const CudaTransitionSliceParams& p,
    uint32_t* iter_u32, float* norm_f32);

CudaTransitionSliceStats cuda_render_transition_slice_metric(
    const CudaTransitionSliceParams& p,
    float* field_f32, float& field_min, float& field_max);

} // namespace fsd_cuda
