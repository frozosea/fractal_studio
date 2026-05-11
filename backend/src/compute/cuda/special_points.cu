// compute/cuda/special_points.cu

#include "special_points.cuh"

#include <cuda_runtime.h>

#include <cmath>
#include <stdexcept>
#include <string>

#define CUDA_CHECK(expr)  do {                                                \
    cudaError_t _e = (expr);                                                  \
    if (_e != cudaSuccess)                                                    \
        throw std::runtime_error(std::string("CUDA special points: ") +       \
            cudaGetErrorString(_e) + " at " __FILE__ ":" +                  \
            std::to_string(__LINE__));                                        \
} while(0)

namespace fsd_cuda {
namespace {

struct DeviceComplex {
    double re;
    double im;
};

__device__ inline DeviceComplex c_add(DeviceComplex a, DeviceComplex b) {
    return {a.re + b.re, a.im + b.im};
}

__device__ inline DeviceComplex c_sub(DeviceComplex a, DeviceComplex b) {
    return {a.re - b.re, a.im - b.im};
}

__device__ inline DeviceComplex c_mul(DeviceComplex a, DeviceComplex b) {
    return {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}

__device__ inline DeviceComplex c_div(DeviceComplex a, DeviceComplex b) {
    const double den = b.re * b.re + b.im * b.im;
    return {
        (a.re * b.re + a.im * b.im) / den,
        (a.im * b.re - a.re * b.im) / den,
    };
}

__device__ inline double c_abs(DeviceComplex z) {
    return hypot(z.re, z.im);
}

__device__ inline bool c_finite(DeviceComplex z) {
    return isfinite(z.re) && isfinite(z.im);
}

__device__ inline void eval_center_f_df_device(
    DeviceComplex c,
    int period,
    DeviceComplex& f,
    DeviceComplex& df
) {
    DeviceComplex z{0.0, 0.0};
    DeviceComplex dz{0.0, 0.0};
    for (int i = 0; i < period; ++i) {
        dz = c_add(c_mul({2.0 * z.re, 2.0 * z.im}, dz), {1.0, 0.0});
        z = c_add(c_mul(z, z), c);
    }
    f = z;
    df = dz;
}

__global__ void center_newton_batch_kernel(
    const CudaCenterSeed* seeds,
    CudaCenterNewtonResult* out,
    int count,
    int period,
    int max_newton_iter,
    double accept_eps
) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= count) return;

    DeviceComplex c{seeds[idx].re, seeds[idx].im};
    CudaCenterNewtonResult result{};
    result.re = c.re;
    result.im = c.im;

    if (!c_finite(c) || c_abs(c) > 4.0) {
        out[idx] = result;
        return;
    }

    int iterations = 0;
    for (int iter = 0; iter < max_newton_iter; ++iter) {
        DeviceComplex f;
        DeviceComplex df;
        eval_center_f_df_device(c, period, f, df);
        const double residual = c_abs(f);
        iterations = iter;

        if (residual < accept_eps) {
            result.converged = 1;
            result.residual = residual;
            break;
        }

        if (c_abs(df) < 1e-20) {
            result.residual = residual;
            break;
        }

        const DeviceComplex step = c_div(f, df);
        c = c_sub(c, step);
        if (!c_finite(c) || c_abs(c) > 4.0) {
            result.residual = residual;
            break;
        }
        if (c_abs(step) < 1e-16 && residual >= accept_eps) {
            result.residual = residual;
            break;
        }
    }

    DeviceComplex f;
    DeviceComplex df;
    eval_center_f_df_device(c, period, f, df);
    (void)df;
    result.re = c.re;
    result.im = c.im;
    result.residual = c_abs(f);
    result.iterations = iterations;
    result.converged = result.residual < accept_eps ? 1 : 0;
    out[idx] = result;
}

} // namespace

bool cuda_special_points_available() noexcept {
    int count = 0;
    return cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

std::vector<CudaCenterNewtonResult> cuda_solve_center_batch(
    const std::vector<CudaCenterSeed>& seeds,
    int period,
    int max_newton_iter,
    double accept_eps
) {
    if (seeds.empty()) return {};

    CudaCenterSeed* d_seeds = nullptr;
    CudaCenterNewtonResult* d_results = nullptr;
    const size_t seed_bytes = seeds.size() * sizeof(CudaCenterSeed);
    const size_t result_bytes = seeds.size() * sizeof(CudaCenterNewtonResult);

    try {
        CUDA_CHECK(cudaMalloc(&d_seeds, seed_bytes));
        CUDA_CHECK(cudaMalloc(&d_results, result_bytes));
        CUDA_CHECK(cudaMemcpy(d_seeds, seeds.data(), seed_bytes, cudaMemcpyHostToDevice));
        const int threads = 128;
        const int blocks = static_cast<int>((seeds.size() + threads - 1) / threads);
        center_newton_batch_kernel<<<blocks, threads>>>(
            d_seeds,
            d_results,
            static_cast<int>(seeds.size()),
            period,
            max_newton_iter,
            accept_eps);
        CUDA_CHECK(cudaGetLastError());
        CUDA_CHECK(cudaDeviceSynchronize());

        std::vector<CudaCenterNewtonResult> results(seeds.size());
        CUDA_CHECK(cudaMemcpy(results.data(), d_results, result_bytes, cudaMemcpyDeviceToHost));
        cudaFree(d_results);
        cudaFree(d_seeds);
        return results;
    } catch (...) {
        cudaFree(d_results);
        cudaFree(d_seeds);
        throw;
    }
}

} // namespace fsd_cuda
