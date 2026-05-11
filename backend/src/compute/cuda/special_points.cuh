// compute/cuda/special_points.cuh
//
// CUDA helpers for special-point Newton batches. The CPU searcher owns
// ordering, classification, visibility filtering, and fallback behavior.

#pragma once

#include <vector>

namespace fsd_cuda {

struct CudaCenterSeed {
    double re = 0.0;
    double im = 0.0;
};

struct CudaCenterNewtonResult {
    double re = 0.0;
    double im = 0.0;
    double residual = 0.0;
    int iterations = 0;
    int converged = 0;
};

bool cuda_special_points_available() noexcept;

std::vector<CudaCenterNewtonResult> cuda_solve_center_batch(
    const std::vector<CudaCenterSeed>& seeds,
    int period,
    int max_newton_iter,
    double accept_eps);

} // namespace fsd_cuda
