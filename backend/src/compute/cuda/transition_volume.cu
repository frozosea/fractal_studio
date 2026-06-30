// compute/cuda/transition_volume.cu

#include "transition_volume.cuh"

#include <cuda_runtime.h>

#include <cmath>
#include <stdexcept>
#include <string>

#define CUDA_CHECK(expr)  do {                                                \
    cudaError_t _e = (expr);                                                  \
    if (_e != cudaSuccess)                                                    \
        throw std::runtime_error(std::string("CUDA transition: ") + cudaGetErrorString(_e)); \
} while(0)

namespace fsd_cuda {

__device__ inline float real_projection(int v, float x2, float axis2) {
    const bool post_abs = v == 5 || v == 6 || v == 7 || v == 8 || v == 9;
    float q = x2 - axis2;
    return post_abs ? fabsf(q) : q;
}

__device__ inline float imag_projection(int v, float x, float axis) {
    const bool abs_x = v == 2 || v == 4 || v == 9;
    const bool abs_axis = v == 2 || v == 3 || v == 7 || v == 8;
    const bool neg = v == 1 || v == 4 || v == 6 || v == 9;
    const float a = abs_x ? fabsf(x) : x;
    const float b = abs_axis ? fabsf(axis) : axis;
    const float q = 2.0f * a * b;
    return neg ? -q : q;
}

__global__ void transition_volume_kernel(CudaTransitionVolumeParams p, int z_start, int z_count, float* out) {
    const int N = p.resolution;
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    const int total = N * N * z_count;
    if (idx >= total) return;

    const int xi = idx % N;
    const int yi = (idx / N) % N;
    const int zi_local = idx / (N * N);
    const int zi = z_start + zi_local;

    const float span = p.extent * 2.0f;
    const float x0 = (p.center_x - p.extent) + (static_cast<float>(xi) + 0.5f) / static_cast<float>(N) * span;
    const float y0 = (p.center_y - p.extent) + (static_cast<float>(yi) + 0.5f) / static_cast<float>(N) * span;
    const float z0 = (p.center_z - p.extent) + (static_cast<float>(zi) + 0.5f) / static_cast<float>(N) * span;

    if (p.multi_count > 0) {
        float x = x0;
        float x2 = x * x;
        float axis[CUDA_MAX_TRANSITION_VOLUME_LEGS];
        float axis2[CUDA_MAX_TRANSITION_VOLUME_LEGS];
        for (int k = 0; k < p.multi_count; ++k) {
            axis[k] = y0 * p.multi_y_factor[k] + z0 * p.multi_z_factor[k];
            axis2[k] = axis[k] * axis[k];
        }

        int iter = 0;
        bool escaped = false;
        for (; iter < p.iterations; ++iter) {
            float real_sum = 0.0f;
            float influence_sum = 0.0f;
            float next_axis[CUDA_MAX_TRANSITION_VOLUME_LEGS];
            for (int k = 0; k < p.multi_count; ++k) {
                const float influence = p.multi_influence[k];
                real_sum += influence * real_projection(p.multi_variants[k], x2, axis2[k]);
                influence_sum += influence;
                const float caxis = y0 * p.multi_y_factor[k] + z0 * p.multi_z_factor[k];
                next_axis[k] = influence * imag_projection(p.multi_variants[k], x, axis[k]) + caxis;
            }

            const float nx = real_sum - (influence_sum - 1.0f) * x2 + x0;
            const bool finite_x = isfinite(nx);
            const float nx2 = finite_x ? nx * nx : INFINITY;
            bool finite_all = finite_x;
            float n2 = finite_x ? nx2 : INFINITY;
            for (int k = 0; k < p.multi_count; ++k) {
                if (!isfinite(next_axis[k])) {
                    finite_all = false;
                    n2 = INFINITY;
                    break;
                }
                n2 += next_axis[k] * next_axis[k];
            }
            if (!finite_all || n2 > p.bailout_sq) {
                escaped = true;
                break;
            }

            x = nx;
            x2 = nx2;
            for (int k = 0; k < p.multi_count; ++k) {
                axis[k] = next_axis[k];
                axis2[k] = axis[k] * axis[k];
            }
        }

        if (escaped) {
            out[idx] = 0.5f + 0.5f * (static_cast<float>(iter) / static_cast<float>(p.iterations));
        } else {
            float mag2 = x2;
            for (int k = 0; k < p.multi_count; ++k) mag2 += axis2[k];
            const float final_mag = sqrtf(mag2);
            out[idx] = fminf(0.48f, final_mag / p.bailout * 0.48f);
        }
        return;
    }

    float x = x0, y = y0, z = z0;
    float x2 = x * x;
    float y2 = y * y;
    float z2 = z * z;
    int iter = 0;
    bool escaped = false;
    for (; iter < p.iterations; ++iter) {
        const float nx = real_projection(p.from_variant, x2, y2)
                       + real_projection(p.to_variant,   x2, z2)
                       - x2 + x0;
        const float ny = imag_projection(p.from_variant, x, y) + y0;
        const float nz = imag_projection(p.to_variant,   x, z) + z0;
        const bool finite = isfinite(nx) && isfinite(ny) && isfinite(nz);
        const float nx2 = finite ? nx * nx : INFINITY;
        const float ny2 = finite ? ny * ny : INFINITY;
        const float nz2 = finite ? nz * nz : INFINITY;
        const float n2 = nx2 + ny2 + nz2;
        if (!finite || n2 > p.bailout_sq) {
            escaped = true;
            break;
        }
        x = nx; y = ny; z = nz;
        x2 = nx2; y2 = ny2; z2 = nz2;
    }

    if (escaped) {
        out[idx] = 0.5f + 0.5f * (static_cast<float>(iter) / static_cast<float>(p.iterations));
    } else {
        const float final_mag = sqrtf(x2 + y2 + z2);
        out[idx] = fminf(0.48f, final_mag / p.bailout * 0.48f);
    }
}

bool cuda_transition_available() noexcept {
    int count = 0;
    return cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

void cuda_build_transition_volume(const CudaTransitionVolumeParams& p, std::vector<float>& out) {
    if (!cuda_transition_available()) throw std::runtime_error("CUDA transition not available");
    const size_t count = static_cast<size_t>(p.resolution) * p.resolution * p.resolution;
    out.assign(count, 1.0f);

    float* d_out = nullptr;
    CUDA_CHECK(cudaMalloc(&d_out, count * sizeof(float)));
    const int block = 256;
    const int grid = static_cast<int>((count + block - 1) / block);
    transition_volume_kernel<<<grid, block>>>(p, 0, p.resolution, d_out);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());
    CUDA_CHECK(cudaMemcpy(out.data(), d_out, count * sizeof(float), cudaMemcpyDeviceToHost));
    cudaFree(d_out);
}

void cuda_build_transition_volume_slabs(const CudaTransitionVolumeParams& p, int z_start, int z_count, std::vector<float>& out) {
    if (!cuda_transition_available()) throw std::runtime_error("CUDA transition not available");
    if (z_start < 0 || z_count <= 0 || z_start + z_count > p.resolution) {
        throw std::runtime_error("invalid CUDA transition slab range");
    }
    const size_t count = static_cast<size_t>(p.resolution) * p.resolution * z_count;
    out.assign(count, 1.0f);

    float* d_out = nullptr;
    CUDA_CHECK(cudaMalloc(&d_out, count * sizeof(float)));
    const int block = 256;
    const int grid = static_cast<int>((count + block - 1) / block);
    transition_volume_kernel<<<grid, block>>>(p, z_start, z_count, d_out);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());
    CUDA_CHECK(cudaMemcpy(out.data(), d_out, count * sizeof(float), cudaMemcpyDeviceToHost));
    cudaFree(d_out);
}

} // namespace fsd_cuda
