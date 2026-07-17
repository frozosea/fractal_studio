// compute/cuda/perturb_kernel.cu
//
// CUDA implementation of the perturbation delta iteration. One thread per
// pixel; the combined reference table lives in global memory (threads march
// through it nearly in lockstep, so reads hit L2/read-only cache).
//
// The delta type is templated: fp64 for the exact deep path, fp32 for the
// consumer-GPU fast path (RTX 40 runs fp32 at 32-64x fp64 throughput; the
// per-pixel offset generation stays fp64 and is amortized over the whole
// iteration loop).

#include "perturb_kernel.cuh"

#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace fsd_cuda {

namespace {

struct DeviceParams {
    int width, height, iterations;
    int row_offset, render_height;
    double bailout_sq;
    int offset_mode;
    double span_re, span_im, cos_t, sin_t;
    double ln_r0, k_step, theta_step;
    bool julia;
    double dz_shift_re, dz_shift_im;
    int start_off, start_len, k_off, k_len;
};

template <typename T>
__global__ void perturb_field_kernel(
    DeviceParams p,
    const T* __restrict__ tab_re,
    const T* __restrict__ tab_im,
    uint32_t* __restrict__ out_iter,
    float* __restrict__ out_norm)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int local_y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= p.width || local_y >= p.render_height) return;
    const int y = p.row_offset + local_y;

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

    T dz_re, dz_im, dc_re, dc_im;
    if (p.julia) {
        dz_re = static_cast<T>(off_re + p.dz_shift_re);
        dz_im = static_cast<T>(off_im + p.dz_shift_im);
        dc_re = T(0);   dc_im = T(0);
    } else {
        dz_re = static_cast<T>(p.dz_shift_re);
        dz_im = static_cast<T>(p.dz_shift_im);
        dc_re = static_cast<T>(off_re);
        dc_im = static_cast<T>(off_im);
    }

    const T bail2 = static_cast<T>(p.bailout_sq);
    const T two = T(2);

    int base = p.start_off;
    int olen = p.start_len;
    int m = 0;

    int iter = p.iterations;
    T escape_mag2 = T(0);

    for (int n = 0; n < p.iterations; ++n) {
        const T two_Zr = two * tab_re[base + m];
        const T two_Zi = two * tab_im[base + m];

        const T ndr = two_Zr * dz_re - two_Zi * dz_im
                    + dz_re * dz_re - dz_im * dz_im + dc_re;
        const T ndi = two_Zr * dz_im + two_Zi * dz_re
                    + two * dz_re * dz_im + dc_im;
        dz_re = ndr;
        dz_im = ndi;

        const T z_re = tab_re[base + m + 1] + dz_re;
        const T z_im = tab_im[base + m + 1] + dz_im;
        const T mag2 = z_re * z_re + z_im * z_im;

        if (mag2 > bail2) {
            iter = n;
            escape_mag2 = mag2;
            break;
        }

        ++m;
        const T dz_mag2 = dz_re * dz_re + dz_im * dz_im;
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

template <typename T>
bool run_perturb_field(
    const CudaPerturbParams& p,
    const T* h_tab_re, const T* h_tab_im,
    uint32_t* iter_u32, float* norm_f32)
{
    const size_t px_count = static_cast<size_t>(p.width) * p.height;
    const size_t tab_bytes = static_cast<size_t>(p.tab_len) * sizeof(T);

    T* d_tab_re = nullptr;
    T* d_tab_im = nullptr;
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

    if (cudaMemcpy(d_tab_re, h_tab_re, tab_bytes, cudaMemcpyHostToDevice) != cudaSuccess) return fail();
    if (cudaMemcpy(d_tab_im, h_tab_im, tab_bytes, cudaMemcpyHostToDevice) != cudaSuccess) return fail();

    DeviceParams dp;
    dp.width = p.width;           dp.height = p.height;
    dp.row_offset = 0;            dp.render_height = p.height;
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

    auto cancel_requested = [&]() noexcept {
        if (p.should_cancel == nullptr || !*p.should_cancel) return false;
        try {
            return (*p.should_cancel)();
        } catch (...) {
            return true;
        }
    };

    const dim3 block(16, 16);
    const int chunk_rows = !p.should_cancel || p.iterations <= 2048
        ? p.height
        : (p.iterations > 262144 ? 16
           : (p.iterations > 32768 ? 64 : 256));
    for (int row = 0; row < p.height; row += chunk_rows) {
        if (cancel_requested()) return fail();
        dp.row_offset = row;
        dp.render_height = std::min(chunk_rows, p.height - row);
        const dim3 grid((p.width + block.x - 1) / block.x,
                        (dp.render_height + block.y - 1) / block.y);
        perturb_field_kernel<T><<<grid, block>>>(
            dp, d_tab_re, d_tab_im, d_iter, d_norm);
        if (cudaGetLastError() != cudaSuccess) return fail();
        if (cudaDeviceSynchronize() != cudaSuccess) return fail();
        if (cancel_requested()) return fail();
    }

    if (cudaMemcpy(iter_u32, d_iter, px_count * sizeof(uint32_t),
                   cudaMemcpyDeviceToHost) != cudaSuccess) return fail();
    if (cudaMemcpy(norm_f32, d_norm, px_count * sizeof(float),
                   cudaMemcpyDeviceToHost) != cudaSuccess) return fail();

    cleanup();
    return true;
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
    bool ok;
    if (p.fp32_delta) {
        std::vector<float> tab32_re(p.tab_re, p.tab_re + p.tab_len);
        std::vector<float> tab32_im(p.tab_im, p.tab_im + p.tab_len);
        ok = run_perturb_field<float>(p, tab32_re.data(), tab32_im.data(),
                                      iter_u32, norm_f32);
    } else {
        ok = run_perturb_field<double>(p, p.tab_re, p.tab_im,
                                       iter_u32, norm_f32);
    }
    if (!ok) return false;

    if (elapsed_ms) {
        *elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    }
    return true;
}

} // namespace fsd_cuda
