// compute/cuda/perturb_kernel.cu
//
// CUDA implementation of the perturbation delta iteration. One thread per
// pixel; the combined reference table lives in global memory (threads march
// through it nearly in lockstep, so reads hit L2/read-only cache).

#include "perturb_kernel.cuh"

#include <cuda_runtime.h>

#include <chrono>

namespace fsd_cuda {

namespace {

struct DeviceParams {
    int width, height, iterations;
    double bailout_sq;
    int offset_mode;
    double span_re, span_im, cos_t, sin_t;
    double ln_r0, k_step, theta_step;
    bool julia;
    double dz_shift_re, dz_shift_im;
    int start_off, start_len, k_off, k_len;
};

__global__ void perturb_field_kernel(
    DeviceParams p,
    const double* __restrict__ tab_re,
    const double* __restrict__ tab_im,
    uint32_t* __restrict__ out_iter,
    float* __restrict__ out_norm)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= p.width || y >= p.height) return;

    double off_re, off_im;
    if (p.offset_mode == 0) {
        const double dx = p.span_re * ((x + 0.5) / p.width - 0.5);
        const double dy = -p.span_im * ((y + 0.5) / p.height - 0.5);
        off_re = dx * p.cos_t - dy * p.sin_t;
        off_im = dx * p.sin_t + dy * p.cos_t;
    } else {
        const double r = exp(p.ln_r0 - y * p.k_step);
        double s, c;
        sincos(x * p.theta_step, &s, &c);
        off_re = r * c;
        off_im = r * s;
    }

    double dz_re, dz_im, dc_re, dc_im;
    if (p.julia) {
        dz_re = off_re + p.dz_shift_re; dz_im = off_im + p.dz_shift_im;
        dc_re = 0.0;    dc_im = 0.0;
    } else {
        dz_re = p.dz_shift_re; dz_im = p.dz_shift_im;
        dc_re = off_re; dc_im = off_im;
    }

    int base = p.start_off;
    int olen = p.start_len;
    int m = 0;

    int iter = p.iterations;
    double escape_mag2 = 0.0;

    for (int n = 0; n < p.iterations; ++n) {
        const double two_Zr = 2.0 * tab_re[base + m];
        const double two_Zi = 2.0 * tab_im[base + m];

        const double ndr = two_Zr * dz_re - two_Zi * dz_im
                         + dz_re * dz_re - dz_im * dz_im + dc_re;
        const double ndi = two_Zr * dz_im + two_Zi * dz_re
                         + 2.0 * dz_re * dz_im + dc_im;
        dz_re = ndr;
        dz_im = ndi;

        const double z_re = tab_re[base + m + 1] + dz_re;
        const double z_im = tab_im[base + m + 1] + dz_im;
        const double mag2 = z_re * z_re + z_im * z_im;

        if (mag2 > p.bailout_sq) {
            iter = n;
            escape_mag2 = mag2;
            break;
        }

        ++m;
        const double dz_mag2 = dz_re * dz_re + dz_im * dz_im;
        if (mag2 < dz_mag2 || m + 1 >= olen) {
            dz_re = z_re;
            dz_im = z_im;
            base = p.k_off;
            olen = p.k_len;
            m = 0;
        }
    }

    const size_t idx = static_cast<size_t>(y) * p.width + x;
    out_iter[idx] = static_cast<uint32_t>(iter);
    out_norm[idx] = iter < p.iterations ? static_cast<float>(escape_mag2) : 0.0f;
}

} // namespace

bool cuda_render_perturb_field(
    const CudaPerturbParams& p,
    uint32_t* iter_u32, float* norm_f32,
    double* elapsed_ms)
{
    if (!p.tab_re || !p.tab_im || p.tab_len < 2 ||
        p.start_len < 2 || p.k_len < 2 ||
        p.start_off + p.start_len > p.tab_len ||
        p.k_off + p.k_len > p.tab_len ||
        p.width <= 0 || p.height <= 0) {
        return false;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const size_t px_count = static_cast<size_t>(p.width) * p.height;
    const size_t tab_bytes = static_cast<size_t>(p.tab_len) * sizeof(double);

    double* d_tab_re = nullptr;
    double* d_tab_im = nullptr;
    uint32_t* d_iter = nullptr;
    float* d_norm = nullptr;

    auto cleanup = [&]() {
        if (d_tab_re) cudaFree(d_tab_re);
        if (d_tab_im) cudaFree(d_tab_im);
        if (d_iter)   cudaFree(d_iter);
        if (d_norm)   cudaFree(d_norm);
    };
    auto fail = [&]() { cleanup(); cudaGetLastError(); return false; };

    if (cudaMalloc(&d_tab_re, tab_bytes) != cudaSuccess) return fail();
    if (cudaMalloc(&d_tab_im, tab_bytes) != cudaSuccess) return fail();
    if (cudaMalloc(&d_iter, px_count * sizeof(uint32_t)) != cudaSuccess) return fail();
    if (cudaMalloc(&d_norm, px_count * sizeof(float)) != cudaSuccess) return fail();

    if (cudaMemcpy(d_tab_re, p.tab_re, tab_bytes, cudaMemcpyHostToDevice) != cudaSuccess) return fail();
    if (cudaMemcpy(d_tab_im, p.tab_im, tab_bytes, cudaMemcpyHostToDevice) != cudaSuccess) return fail();

    DeviceParams dp;
    dp.width = p.width;           dp.height = p.height;
    dp.iterations = p.iterations; dp.bailout_sq = p.bailout_sq;
    dp.offset_mode = p.offset_mode;
    dp.span_re = p.span_re;       dp.span_im = p.span_im;
    dp.cos_t = p.cos_t;           dp.sin_t = p.sin_t;
    dp.ln_r0 = p.ln_r0;           dp.k_step = p.k_step;
    dp.theta_step = p.theta_step; dp.julia = p.julia;
    dp.dz_shift_re = p.dz_shift_re;
    dp.dz_shift_im = p.dz_shift_im;
    dp.start_off = p.start_off;   dp.start_len = p.start_len;
    dp.k_off = p.k_off;           dp.k_len = p.k_len;

    const dim3 block(16, 16);
    const dim3 grid((p.width + block.x - 1) / block.x,
                    (p.height + block.y - 1) / block.y);
    perturb_field_kernel<<<grid, block>>>(dp, d_tab_re, d_tab_im, d_iter, d_norm);
    if (cudaGetLastError() != cudaSuccess) return fail();
    if (cudaDeviceSynchronize() != cudaSuccess) return fail();

    if (cudaMemcpy(iter_u32, d_iter, px_count * sizeof(uint32_t),
                   cudaMemcpyDeviceToHost) != cudaSuccess) return fail();
    if (cudaMemcpy(norm_f32, d_norm, px_count * sizeof(float),
                   cudaMemcpyDeviceToHost) != cudaSuccess) return fail();

    cleanup();
    if (elapsed_ms) {
        *elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    }
    return true;
}

} // namespace fsd_cuda
