// compute/cuda/transition_kernel.cu
//
// CUDA 2D transition slice renderer.  Follows the runtime variant dispatch
// from transition_volume.cu and the thread/buffer layout from map_kernel.cu.

#include "transition_kernel.cuh"

#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>

#define CUDA_CHECK(expr) do {                                                 \
    cudaError_t _e = (expr);                                                  \
    if (_e != cudaSuccess)                                                    \
        throw std::runtime_error(std::string("CUDA transition slice: ")       \
                                 + cudaGetErrorString(_e));                   \
} while(0)

namespace fsd_cuda {

// ── Variant fold helpers (shared with transition_volume.cu pattern) ──────────

__device__ inline double real_projection_d(int v, double x2, double axis2) {
    const bool post_abs = v == 5 || v == 6 || v == 7 || v == 8 || v == 9;
    double q = x2 - axis2;
    return post_abs ? fabs(q) : q;
}

__device__ inline double imag_projection_d(int v, double x, double axis) {
    const bool abs_x    = v == 2 || v == 4 || v == 9;
    const bool abs_axis = v == 2 || v == 3 || v == 7 || v == 8;
    const bool neg      = v == 1 || v == 4 || v == 6 || v == 9;
    const double a = abs_x    ? fabs(x)    : x;
    const double b = abs_axis ? fabs(axis) : axis;
    const double q = 2.0 * a * b;
    return neg ? -q : q;
}

__device__ inline float real_projection_f(int v, float x2, float axis2) {
    const bool post_abs = v == 5 || v == 6 || v == 7 || v == 8 || v == 9;
    float q = x2 - axis2;
    return post_abs ? fabsf(q) : q;
}

__device__ inline float imag_projection_f(int v, float x, float axis) {
    const bool abs_x    = v == 2 || v == 4 || v == 9;
    const bool abs_axis = v == 2 || v == 3 || v == 7 || v == 8;
    const bool neg      = v == 1 || v == 4 || v == 6 || v == 9;
    const float a = abs_x    ? fabsf(x)    : x;
    const float b = abs_axis ? fabsf(axis) : axis;
    const float q = 2.0f * a * b;
    return neg ? -q : q;
}

// ── Escape metric kernels ───────────────────────────────────────────────────

__global__ void transition_slice_escape_fp64(
    double center_re, double center_im, double scale,
    int W, int H, int max_iter, double bail2,
    double cth, double sth,
    int from_v, int to_v,
    bool julia, double jre, double jim,
    uint32_t* __restrict__ out_iter, float* __restrict__ out_norm
) {
    const int px = blockIdx.x * blockDim.x + threadIdx.x;
    const int py = blockIdx.y * blockDim.y + threadIdx.y;
    if (px >= W || py >= H) return;

    const double aspect  = static_cast<double>(W) / static_cast<double>(H);
    const double span_im = scale;
    const double span_re = scale * aspect;
    const double u = (center_re - span_re * 0.5) +
                     (static_cast<double>(px) + 0.5) / W * span_re;
    const double v = (center_im + span_im * 0.5) -
                     (static_cast<double>(py) + 0.5) / H * span_im;

    double x0 = u,        y0 = v * cth, z0 = v * sth;
    double cx = julia ? jre       : x0;
    double cy = julia ? jim * cth : y0;
    double cz = julia ? jim * sth : z0;

    double x = x0, y = y0, z = z0;
    double x2 = x*x, y2 = y*y, z2 = z*z;
    double esc_norm = 0.0;

    int i = 0;
    for (; i < max_iter; i++) {
        const double nx = real_projection_d(from_v, x2, y2)
                        + real_projection_d(to_v,   x2, z2)
                        - x2 + cx;
        const double ny = imag_projection_d(from_v, x, y) + cy;
        const double nz = imag_projection_d(to_v,   x, z) + cz;
        const bool fin = isfinite(nx) && isfinite(ny) && isfinite(nz);
        const double nx2 = fin ? nx*nx : INFINITY;
        const double ny2 = fin ? ny*ny : INFINITY;
        const double nz2 = fin ? nz*nz : INFINITY;
        const double n2  = nx2 + ny2 + nz2;
        if (!fin || n2 > bail2) { esc_norm = n2; break; }
        x = nx; y = ny; z = nz;
        x2 = nx2; y2 = ny2; z2 = nz2;
    }

    const size_t idx = static_cast<size_t>(py) * W + px;
    out_iter[idx] = static_cast<uint32_t>(i);
    out_norm[idx] = static_cast<float>(esc_norm);
}

__global__ void transition_slice_escape_fp32(
    float center_re, float center_im, float scale,
    int W, int H, int max_iter, float bail2,
    float cth, float sth,
    int from_v, int to_v,
    bool julia, float jre, float jim,
    uint32_t* __restrict__ out_iter, float* __restrict__ out_norm
) {
    const int px = blockIdx.x * blockDim.x + threadIdx.x;
    const int py = blockIdx.y * blockDim.y + threadIdx.y;
    if (px >= W || py >= H) return;

    const float aspect  = static_cast<float>(W) / static_cast<float>(H);
    const float span_im = scale;
    const float span_re = scale * aspect;
    const float u = (center_re - span_re * 0.5f) +
                    (static_cast<float>(px) + 0.5f) / static_cast<float>(W) * span_re;
    const float v = (center_im + span_im * 0.5f) -
                    (static_cast<float>(py) + 0.5f) / static_cast<float>(H) * span_im;

    float x0 = u,        y0 = v * cth, z0 = v * sth;
    float cx = julia ? jre       : x0;
    float cy = julia ? jim * cth : y0;
    float cz = julia ? jim * sth : z0;

    float x = x0, y = y0, z = z0;
    float x2 = x*x, y2 = y*y, z2 = z*z;
    float esc_norm = 0.0f;

    int i = 0;
    for (; i < max_iter; i++) {
        const float nx = real_projection_f(from_v, x2, y2)
                       + real_projection_f(to_v,   x2, z2)
                       - x2 + cx;
        const float ny = imag_projection_f(from_v, x, y) + cy;
        const float nz = imag_projection_f(to_v,   x, z) + cz;
        const bool fin = isfinite(nx) && isfinite(ny) && isfinite(nz);
        const float nx2 = fin ? nx*nx : INFINITY;
        const float ny2 = fin ? ny*ny : INFINITY;
        const float nz2 = fin ? nz*nz : INFINITY;
        const float n2  = nx2 + ny2 + nz2;
        if (!fin || n2 > bail2) { esc_norm = n2; break; }
        x = nx; y = ny; z = nz;
        x2 = nx2; y2 = ny2; z2 = nz2;
    }

    const size_t idx = static_cast<size_t>(py) * W + px;
    out_iter[idx] = static_cast<uint32_t>(i);
    out_norm[idx] = static_cast<float>(esc_norm);
}

// ── Non-escape metric kernels (MinAbs/MaxAbs/Envelope) ──────────────────────

__global__ void transition_slice_metric_fp64(
    double center_re, double center_im, double scale,
    int W, int H, int max_iter, double bail2,
    double cth, double sth,
    int from_v, int to_v, int metric_id,
    bool julia, double jre, double jim,
    float* __restrict__ out_field
) {
    const int px = blockIdx.x * blockDim.x + threadIdx.x;
    const int py = blockIdx.y * blockDim.y + threadIdx.y;
    if (px >= W || py >= H) return;

    const double aspect  = static_cast<double>(W) / static_cast<double>(H);
    const double span_im = scale;
    const double span_re = scale * aspect;
    const double u = (center_re - span_re * 0.5) +
                     (static_cast<double>(px) + 0.5) / W * span_re;
    const double v = (center_im + span_im * 0.5) -
                     (static_cast<double>(py) + 0.5) / H * span_im;

    double x0 = u,        y0 = v * cth, z0 = v * sth;
    double cx = julia ? jre       : x0;
    double cy = julia ? jim * cth : y0;
    double cz = julia ? jim * sth : z0;

    double x = x0, y = y0, z = z0;
    double x2 = x*x, y2 = y*y, z2 = z*z;
    double min_abs_sq = x2 + y2 + z2;
    double max_abs_sq = min_abs_sq;

    for (int i = 0; i < max_iter; i++) {
        const double nx = real_projection_d(from_v, x2, y2)
                        + real_projection_d(to_v,   x2, z2)
                        - x2 + cx;
        const double ny = imag_projection_d(from_v, x, y) + cy;
        const double nz = imag_projection_d(to_v,   x, z) + cz;
        const bool fin = isfinite(nx) && isfinite(ny) && isfinite(nz);
        const double n2 = fin ? (nx*nx + ny*ny + nz*nz) : INFINITY;
        if (n2 < min_abs_sq) min_abs_sq = n2;
        if (n2 > max_abs_sq) max_abs_sq = n2;
        if (!fin || n2 > bail2) break;
        x = nx; y = ny; z = nz;
        x2 = nx*nx; y2 = ny*ny; z2 = nz*nz;
    }

    double val = 0.0;
    if      (metric_id == 1) val = sqrt(min_abs_sq);                                  // MinAbs
    else if (metric_id == 2) val = sqrt(max_abs_sq);                                  // MaxAbs
    else if (metric_id == 3) val = 0.5 * (sqrt(min_abs_sq) + sqrt(max_abs_sq));       // Envelope

    const size_t idx = static_cast<size_t>(py) * W + px;
    out_field[idx] = static_cast<float>(val);
}

// ── Device buffer ───────────────────────────────────────────────────────────

struct TransitionFieldDevBuf {
    int W = 0, H = 0;
    uint32_t* d_iter = nullptr;
    float*    d_norm = nullptr;

    void ensure(int w, int h) {
        if (w == W && h == H) return;
        release();
        CUDA_CHECK(cudaMalloc(&d_iter, static_cast<size_t>(w) * h * sizeof(uint32_t)));
        CUDA_CHECK(cudaMalloc(&d_norm, static_cast<size_t>(w) * h * sizeof(float)));
        W = w; H = h;
    }
    void release() {
        if (d_iter) { cudaFree(d_iter); d_iter = nullptr; }
        if (d_norm) { cudaFree(d_norm); d_norm = nullptr; }
        W = H = 0;
    }
};

static std::mutex g_transition_mutex;
static TransitionFieldDevBuf g_transition_devbuf;

// ── Public API ──────────────────────────────────────────────────────────────

bool cuda_transition_slice_available() noexcept {
    int count = 0;
    return cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

CudaTransitionSliceStats cuda_render_transition_slice_escape(
    const CudaTransitionSliceParams& p,
    uint32_t* iter_u32, float* norm_f32
) {
    if (!cuda_transition_slice_available())
        throw std::runtime_error("CUDA transition slice not available");

    const bool use_fp32 = (p.scalar_type == "fp32" || p.scalar_type == "float32" || p.scalar_type == "float");

    std::lock_guard<std::mutex> lock(g_transition_mutex);
    g_transition_devbuf.ensure(p.width, p.height);

    const dim3 block(32, 8);
    const dim3 grid(
        (p.width  + static_cast<int>(block.x) - 1) / static_cast<int>(block.x),
        (p.height + static_cast<int>(block.y) - 1) / static_cast<int>(block.y));

    cudaEvent_t ev0, ev1;
    CUDA_CHECK(cudaEventCreate(&ev0));
    CUDA_CHECK(cudaEventCreate(&ev1));
    CUDA_CHECK(cudaEventRecord(ev0));

    if (use_fp32) {
        transition_slice_escape_fp32<<<grid, block>>>(
            static_cast<float>(p.center_re), static_cast<float>(p.center_im),
            static_cast<float>(p.scale),
            p.width, p.height, p.iterations, static_cast<float>(p.bailout_sq),
            static_cast<float>(p.cos_theta), static_cast<float>(p.sin_theta),
            p.from_variant, p.to_variant,
            p.julia, static_cast<float>(p.julia_re), static_cast<float>(p.julia_im),
            g_transition_devbuf.d_iter, g_transition_devbuf.d_norm);
    } else {
        transition_slice_escape_fp64<<<grid, block>>>(
            p.center_re, p.center_im, p.scale,
            p.width, p.height, p.iterations, p.bailout_sq,
            p.cos_theta, p.sin_theta,
            p.from_variant, p.to_variant,
            p.julia, p.julia_re, p.julia_im,
            g_transition_devbuf.d_iter, g_transition_devbuf.d_norm);
    }

    CUDA_CHECK(cudaEventRecord(ev1));
    CUDA_CHECK(cudaEventSynchronize(ev1));

    float ms = 0.0f;
    CUDA_CHECK(cudaEventElapsedTime(&ms, ev0, ev1));
    CUDA_CHECK(cudaEventDestroy(ev0));
    CUDA_CHECK(cudaEventDestroy(ev1));

    const size_t n = static_cast<size_t>(p.width) * p.height;
    CUDA_CHECK(cudaMemcpy(iter_u32, g_transition_devbuf.d_iter, n * sizeof(uint32_t), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(norm_f32, g_transition_devbuf.d_norm, n * sizeof(float),    cudaMemcpyDeviceToHost));

    CudaTransitionSliceStats s;
    s.elapsed_ms  = static_cast<double>(ms);
    s.scalar_used = use_fp32 ? "fp32" : "fp64";
    return s;
}

CudaTransitionSliceStats cuda_render_transition_slice_metric(
    const CudaTransitionSliceParams& p,
    float* field_f32, float& field_min, float& field_max
) {
    if (!cuda_transition_slice_available())
        throw std::runtime_error("CUDA transition slice not available");

    std::lock_guard<std::mutex> lock(g_transition_mutex);
    g_transition_devbuf.ensure(p.width, p.height);

    const dim3 block(32, 8);
    const dim3 grid(
        (p.width  + static_cast<int>(block.x) - 1) / static_cast<int>(block.x),
        (p.height + static_cast<int>(block.y) - 1) / static_cast<int>(block.y));

    cudaEvent_t ev0, ev1;
    CUDA_CHECK(cudaEventCreate(&ev0));
    CUDA_CHECK(cudaEventCreate(&ev1));
    CUDA_CHECK(cudaEventRecord(ev0));

    // Reuse d_norm buffer for metric field (same size: float per pixel)
    transition_slice_metric_fp64<<<grid, block>>>(
        p.center_re, p.center_im, p.scale,
        p.width, p.height, p.iterations, p.bailout_sq,
        p.cos_theta, p.sin_theta,
        p.from_variant, p.to_variant, p.metric_id,
        p.julia, p.julia_re, p.julia_im,
        g_transition_devbuf.d_norm);

    CUDA_CHECK(cudaEventRecord(ev1));
    CUDA_CHECK(cudaEventSynchronize(ev1));

    float ms = 0.0f;
    CUDA_CHECK(cudaEventElapsedTime(&ms, ev0, ev1));
    CUDA_CHECK(cudaEventDestroy(ev0));
    CUDA_CHECK(cudaEventDestroy(ev1));

    const size_t n = static_cast<size_t>(p.width) * p.height;
    CUDA_CHECK(cudaMemcpy(field_f32, g_transition_devbuf.d_norm, n * sizeof(float), cudaMemcpyDeviceToHost));

    // Host-side min/max scan
    field_min =  std::numeric_limits<float>::infinity();
    field_max = -std::numeric_limits<float>::infinity();
    for (size_t i = 0; i < n; i++) {
        if (field_f32[i] < field_min) field_min = field_f32[i];
        if (field_f32[i] > field_max) field_max = field_f32[i];
    }

    CudaTransitionSliceStats s;
    s.elapsed_ms  = static_cast<double>(ms);
    s.scalar_used = "fp64";
    return s;
}

} // namespace fsd_cuda
