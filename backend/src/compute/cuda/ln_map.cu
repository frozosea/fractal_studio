// compute/cuda/ln_map.cu

#include "ln_map.cuh"

#include "fx64.cuh"

#include <cuda_runtime.h>
#include <opencv2/core.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#define CUDA_LN_CHECK(expr) do {                                             \
    cudaError_t _e = (expr);                                                 \
    if (_e != cudaSuccess)                                                   \
        throw std::runtime_error(std::string("CUDA ln-map: ") + cudaGetErrorString(_e)); \
} while (0)

namespace fsd_cuda {
namespace {

constexpr double TAU = 6.283185307179586;
constexpr double LN_FOUR = 1.3862943611198906;

std::mutex g_ln_mu;
// Progress rendering calls CUDA by row chunks; cache those buffers without
// retaining very large full-strip allocations after one-off renders.
constexpr size_t CACHED_LN_DEVBUF_MAX_BYTES = 128ull * 1024ull * 1024ull;

struct LnDevBuf {
    size_t capacity = 0;
    unsigned char* d = nullptr;

    void ensure(size_t bytes) {
        if (bytes <= capacity) return;
        if (d) {
            cudaFree(d);
            d = nullptr;
            capacity = 0;
        }
        CUDA_LN_CHECK(cudaMalloc(reinterpret_cast<void**>(&d), bytes));
        capacity = bytes;
    }
};

struct LnCudaEvents {
    cudaEvent_t start = nullptr;
    cudaEvent_t stop = nullptr;

    void ensure() {
        if (!start) CUDA_LN_CHECK(cudaEventCreate(&start));
        if (!stop) CUDA_LN_CHECK(cudaEventCreate(&stop));
    }
};

struct ScopedDevicePtr {
    unsigned char* ptr = nullptr;

    ~ScopedDevicePtr() {
        if (ptr) cudaFree(ptr);
    }
};

LnDevBuf g_ln_devbuf;
LnCudaEvents g_ln_events;

__device__ inline int d_clamp255(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

__device__ inline double d_cos_color(double n, double freq) {
    constexpr double PI = 3.141592653589793;
    return 128.0 - 128.0 * cos(freq * n * PI);
}

__device__ inline void d_hsv_to_rgb(double h, double s, double v, int& r, int& g, int& b) {
    const double c = v * s;
    const double hh = h / 60.0;
    const double x = c * (1.0 - fabs(fmod(hh, 2.0) - 1.0));
    double rr = 0.0, gg = 0.0, bb = 0.0;
    if      (hh < 1.0) { rr = c; gg = x; }
    else if (hh < 2.0) { rr = x; gg = c; }
    else if (hh < 3.0) { gg = c; bb = x; }
    else if (hh < 4.0) { gg = x; bb = c; }
    else if (hh < 5.0) { rr = x; bb = c; }
    else               { rr = c; bb = x; }
    const double m = v - c;
    r = d_clamp255(static_cast<int>((rr + m) * 255.0));
    g = d_clamp255(static_cast<int>((gg + m) * 255.0));
    b = d_clamp255(static_cast<int>((bb + m) * 255.0));
}

__device__ inline void d_gradient_set(double t, double a, double z,
                                      int r0, int g0, int b0,
                                      int r1, int g1, int b1,
                                      unsigned char* px) {
    double u = (z > a) ? (t - a) / (z - a) : 0.0;
    if (u < 0.0) u = 0.0;
    if (u > 1.0) u = 1.0;
    const double v = 1.0 - u;
    px[2] = static_cast<unsigned char>(d_clamp255(static_cast<int>(r0 * v + r1 * u + 0.5)));
    px[1] = static_cast<unsigned char>(d_clamp255(static_cast<int>(g0 * v + g1 * u + 0.5)));
    px[0] = static_cast<unsigned char>(d_clamp255(static_cast<int>(b0 * v + b1 * u + 0.5)));
}

__device__ inline bool d_gradient_palette_bgr(double t, int cmap, unsigned char* px) {
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    switch (cmap) {
        case 6:
            if (t <= 0.14) { d_gradient_set(t, 0.00, 0.14,   0,   0,   4,  31,  12,  72, px); return true; }
            if (t <= 0.28) { d_gradient_set(t, 0.14, 0.28,  31,  12,  72,  85,  15, 109, px); return true; }
            if (t <= 0.42) { d_gradient_set(t, 0.28, 0.42,  85,  15, 109, 136,  34, 106, px); return true; }
            if (t <= 0.56) { d_gradient_set(t, 0.42, 0.56, 136,  34, 106, 186,  54,  85, px); return true; }
            if (t <= 0.70) { d_gradient_set(t, 0.56, 0.70, 186,  54,  85, 227,  89,  51, px); return true; }
            if (t <= 0.84) { d_gradient_set(t, 0.70, 0.84, 227,  89,  51, 249, 140,  10, px); return true; }
            if (t <= 0.94) { d_gradient_set(t, 0.84, 0.94, 249, 140,  10, 252, 195,  55, px); return true; }
            d_gradient_set(t, 0.94, 1.00, 252, 195,  55, 252, 255, 164, px); return true;
        case 7:
            if (t <= 0.25) { d_gradient_set(t, 0.00, 0.25,  68,   1,  84,  59,  82, 139, px); return true; }
            if (t <= 0.50) { d_gradient_set(t, 0.25, 0.50,  59,  82, 139,  33, 145, 140, px); return true; }
            if (t <= 0.75) { d_gradient_set(t, 0.50, 0.75,  33, 145, 140,  94, 201,  98, px); return true; }
            d_gradient_set(t, 0.75, 1.00,  94, 201,  98, 253, 231,  37, px); return true;
        case 8:
            if (t <= 0.18) { d_gradient_set(t, 0.00, 0.18,  32,  24,  70,  63,  92, 180, px); return true; }
            if (t <= 0.36) { d_gradient_set(t, 0.18, 0.36,  63,  92, 180,  58, 150, 165, px); return true; }
            if (t <= 0.54) { d_gradient_set(t, 0.36, 0.54,  58, 150, 165, 240, 210, 120, px); return true; }
            if (t <= 0.72) { d_gradient_set(t, 0.54, 0.72, 240, 210, 120, 210,  90,  90, px); return true; }
            if (t <= 0.88) { d_gradient_set(t, 0.72, 0.88, 210,  90,  90,  90,  50, 110, px); return true; }
            d_gradient_set(t, 0.88, 1.00,  90,  50, 110,  32,  24,  70, px); return true;
        case 9:
            if (t <= 0.22) { d_gradient_set(t, 0.00, 0.22,   5,   8,  32,  10,  70, 120, px); return true; }
            if (t <= 0.48) { d_gradient_set(t, 0.22, 0.48,  10,  70, 120,  55, 190, 185, px); return true; }
            if (t <= 0.72) { d_gradient_set(t, 0.48, 0.72,  55, 190, 185, 245, 172,  75, px); return true; }
            d_gradient_set(t, 0.72, 1.00, 245, 172,  75, 255, 246, 210, px); return true;
        default:
            return false;
    }
}

// 1530-step fully-saturated cyclic hue wheel, GREEN at index 0 (matches colormap.hpp hue1530).
__device__ inline void d_hue1530(int idx, unsigned char* px) {
    idx %= 1530;
    if (idx < 0) idx += 1530;
    const int seg = idx / 255;
    const int d   = idx % 255;
    int rr = 0, gg = 0, bb = 0;
    switch (seg) {
        case 0:  rr = 0;       gg = 255;     bb = d;       break; // G -> C
        case 1:  rr = 0;       gg = 255 - d; bb = 255;     break; // C -> B
        case 2:  rr = d;       gg = 0;       bb = 255;     break; // B -> P
        case 3:  rr = 255;     gg = 0;       bb = 255 - d; break; // P -> R
        case 4:  rr = 255;     gg = d;       bb = 0;       break; // R -> Y
        default: rr = 255 - d; gg = 255;     bb = 0;       break; // Y -> G
    }
    px[2] = static_cast<unsigned char>(rr);
    px[1] = static_cast<unsigned char>(gg);
    px[0] = static_cast<unsigned char>(bb);
}

__device__ inline void d_colorize_escape_bgr(int iter, int max_iter, int cmap, unsigned char* px) {
    if (iter >= max_iter) {
        px[0] = px[1] = px[2] = 255;
        return;
    }
    const double n = (static_cast<double>(iter) + 1.0) / (static_cast<double>(max_iter) + 2.0);
    if (d_gradient_palette_bgr(n, cmap, px)) return;
    switch (cmap) {
        case 1:
            px[2] = static_cast<unsigned char>(d_clamp255(iter % 256));
            px[1] = static_cast<unsigned char>(d_clamp255(iter / 256));
            px[0] = static_cast<unsigned char>(d_clamp255((iter % 17) * 17));
            return;
        case 2: {
            const double h = fmod(static_cast<double>(iter), 1440.0) / 4.0;
            int r = 0, g = 0, b = 0;
            d_hsv_to_rgb(h, 1.0, 1.0, r, g, b);
            px[2] = static_cast<unsigned char>(r);
            px[1] = static_cast<unsigned char>(g);
            px[0] = static_cast<unsigned char>(b);
            return;
        }
        case 3: {
            const int m = iter % 765;
            const int band = m / 255;
            const int d = m % 255;
            int r = 255, g = 255, b = 255;
            if      (band == 0) { r = 255 - d; g = d;       b = 255;     }
            else if (band == 1) { r = d;       g = 255;     b = 255 - d; }
            else                { r = 255;     g = 255 - d; b = d;       }
            px[2] = static_cast<unsigned char>(d_clamp255(r));
            px[1] = static_cast<unsigned char>(d_clamp255(g));
            px[0] = static_cast<unsigned char>(d_clamp255(b));
            return;
        }
        case 4: {
            const int v = d_clamp255(static_cast<int>(n * 255.0));
            px[0] = px[1] = px[2] = static_cast<unsigned char>(v);
            return;
        }
        case 10: {  // Spectral1530 — black→green for first 255 iters, then the 1530 wheel
            if (iter < 255) { px[0] = 0; px[1] = static_cast<unsigned char>(iter); px[2] = 0; }
            else d_hue1530((iter - 255) % 1530, px);
            return;
        }
        default:
            px[2] = static_cast<unsigned char>(d_clamp255(static_cast<int>(d_cos_color(n, 53.0))));
            px[1] = static_cast<unsigned char>(d_clamp255(static_cast<int>(d_cos_color(n, 27.0))));
            px[0] = static_cast<unsigned char>(d_clamp255(static_cast<int>(d_cos_color(n, 139.0))));
            return;
    }
}

template <int VariantId>
__device__ inline void d_step_cached(
    double zr,
    double zi,
    double zr2,
    double zi2,
    double cr,
    double ci,
    double& nr,
    double& ni
) {
    const double sq_re = zr2 - zi2;
    const double sq_im = 2.0 * zr * zi;
    if constexpr (VariantId == 1) {
        nr = sq_re + cr;
        ni = ci - sq_im;
    } else if constexpr (VariantId == 2) {
        nr = sq_re + cr;
        ni = 2.0 * fabs(zr) * fabs(zi) + ci;
    } else if constexpr (VariantId == 3) {
        nr = sq_re + cr;
        ni = 2.0 * zr * fabs(zi) + ci;
    } else if constexpr (VariantId == 4) {
        nr = sq_re + cr;
        ni = -2.0 * fabs(zr) * zi + ci;
    } else if constexpr (VariantId == 5) {
        nr = fabs(sq_re) + cr;
        ni = sq_im + ci;
    } else if constexpr (VariantId == 6) {
        nr = fabs(sq_re) + cr;
        ni = ci - sq_im;
    } else if constexpr (VariantId == 7) {
        nr = fabs(sq_re) + cr;
        ni = fabs(sq_im) + ci;
    } else if constexpr (VariantId == 8) {
        nr = fabs(sq_re) + cr;
        ni = 2.0 * zr * fabs(zi) + ci;
    } else if constexpr (VariantId == 9) {
        nr = fabs(sq_re) + cr;
        ni = ci - 2.0 * fabs(zr) * zi;
    } else {
        nr = sq_re + cr;
        ni = sq_im + ci;
    }
}

template <int VariantId>
__device__ inline void d_step_cached_f(
    float zr,
    float zi,
    float zr2,
    float zi2,
    float cr,
    float ci,
    float& nr,
    float& ni
) {
    const float sq_re = zr2 - zi2;
    const float sq_im = 2.0f * zr * zi;
    if constexpr (VariantId == 1) {
        nr = sq_re + cr;
        ni = ci - sq_im;
    } else if constexpr (VariantId == 2) {
        nr = sq_re + cr;
        ni = 2.0f * fabsf(zr) * fabsf(zi) + ci;
    } else if constexpr (VariantId == 3) {
        nr = sq_re + cr;
        ni = 2.0f * zr * fabsf(zi) + ci;
    } else if constexpr (VariantId == 4) {
        nr = sq_re + cr;
        ni = -2.0f * fabsf(zr) * zi + ci;
    } else if constexpr (VariantId == 5) {
        nr = fabsf(sq_re) + cr;
        ni = sq_im + ci;
    } else if constexpr (VariantId == 6) {
        nr = fabsf(sq_re) + cr;
        ni = ci - sq_im;
    } else if constexpr (VariantId == 7) {
        nr = fabsf(sq_re) + cr;
        ni = fabsf(sq_im) + ci;
    } else if constexpr (VariantId == 8) {
        nr = fabsf(sq_re) + cr;
        ni = 2.0f * zr * fabsf(zi) + ci;
    } else if constexpr (VariantId == 9) {
        nr = fabsf(sq_re) + cr;
        ni = ci - 2.0f * fabsf(zr) * zi;
    } else {
        nr = sq_re + cr;
        ni = sq_im + ci;
    }
}

template <int FRAC>
__device__ inline Fixed64<FRAC> fixed_abs(Fixed64<FRAC> x) {
    if (x.raw >= 0) return x;
    return {x.raw == INT64_MIN ? INT64_MAX : -x.raw};
}

__device__ inline uint64_t add_u64_sat_cuda(uint64_t a, uint64_t b) {
    const uint64_t sum = a + b;
    return sum < a ? UINT64_MAX : sum;
}

__device__ inline int64_t u64_to_i64_sat_cuda(uint64_t value) {
    return value > static_cast<uint64_t>(INT64_MAX)
        ? INT64_MAX
        : static_cast<int64_t>(value);
}

template <int FRAC>
__device__ inline uint64_t fixed_positive_raw(double value) {
    const int64_t raw = Fixed64<FRAC>::from_double(value).raw;
    return raw > 0 ? static_cast<uint64_t>(raw) : 0ULL;
}

template <int FRAC, int VariantId>
__device__ inline void d_step_fixed_cached(
    Fixed64<FRAC> zre,
    Fixed64<FRAC> zim,
    Fixed64<FRAC> zre2,
    Fixed64<FRAC> zim2,
    const Fixed64<FRAC>& cre,
    const Fixed64<FRAC>& cim,
    Fixed64<FRAC>& nr,
    Fixed64<FRAC>& ni
) {
    using S = Fixed64<FRAC>;
    const S sq_re = zre2 - zim2;
    if constexpr (VariantId == 1) {
        const S sq_im = (zre * zim) + (zre * zim);
        nr = sq_re + cre;
        ni = cim - sq_im;
    } else if constexpr (VariantId == 2) {
        const S ax = fixed_abs<FRAC>(zre);
        const S ay = fixed_abs<FRAC>(zim);
        const S sq_im = (ax * ay) + (ax * ay);
        nr = sq_re + cre;
        ni = sq_im + cim;
    } else if constexpr (VariantId == 3) {
        const S ay = fixed_abs<FRAC>(zim);
        const S sq_im = (zre * ay) + (zre * ay);
        nr = sq_re + cre;
        ni = sq_im + cim;
    } else if constexpr (VariantId == 4) {
        const S ax = fixed_abs<FRAC>(zre);
        const S sq_im = (ax * (-zim)) + (ax * (-zim));
        nr = sq_re + cre;
        ni = sq_im + cim;
    } else if constexpr (VariantId == 5) {
        const S sq_im = (zre * zim) + (zre * zim);
        nr = fixed_abs<FRAC>(sq_re) + cre;
        ni = sq_im + cim;
    } else if constexpr (VariantId == 6) {
        const S sq_im = (zre * zim) + (zre * zim);
        nr = fixed_abs<FRAC>(sq_re) + cre;
        ni = cim - sq_im;
    } else if constexpr (VariantId == 7) {
        const S sq_im = (zre * zim) + (zre * zim);
        nr = fixed_abs<FRAC>(sq_re) + cre;
        ni = fixed_abs<FRAC>(sq_im) + cim;
    } else if constexpr (VariantId == 8) {
        const S ay = fixed_abs<FRAC>(zim);
        const S sq_im = (zre * ay) + (zre * ay);
        nr = fixed_abs<FRAC>(sq_re) + cre;
        ni = sq_im + cim;
    } else if constexpr (VariantId == 9) {
        const S ax = fixed_abs<FRAC>(zre);
        const S sq_im = (ax * zim) + (ax * zim);
        nr = fixed_abs<FRAC>(sq_re) + cre;
        ni = cim - sq_im;
    } else {
        const S sq_im = (zre * zim) + (zre * zim);
        nr = sq_re + cre;
        ni = sq_im + cim;
    }
}

template <int VariantId>
__global__ void ln_map_kernel_templated(CudaLnMapParams p, int row_start, int row_count, unsigned char* out) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    const int total = p.width_s * row_count;
    if (idx >= total) return;
    const int x = idx % p.width_s;
    const int local_row = idx / p.width_s;
    const int row = row_start + local_row;

    const double th = TAU * static_cast<double>(x) / static_cast<double>(p.width_s);
    const double global_row = p.row_offset + static_cast<double>(row);
    const double k = LN_FOUR - global_row * TAU / static_cast<double>(p.width_s);
    const double r_mag = exp(k);
    const double pre = p.center_re + r_mag * cos(th);
    const double pim = p.center_im + r_mag * sin(th);

    double zr = p.julia ? pre : 0.0;
    double zi = p.julia ? pim : 0.0;
    const double cr = p.julia ? p.julia_re : pre;
    const double ci = p.julia ? p.julia_im : pim;
    double zr2 = zr * zr;
    double zi2 = zi * zi;

    int iter = 0;
    for (; iter < p.iterations; ++iter) {
        double nr = 0.0, ni = 0.0;
        d_step_cached<VariantId>(zr, zi, zr2, zi2, cr, ci, nr, ni);
        const bool finite = isfinite(nr) && isfinite(ni);
        const double nr2 = finite ? nr * nr : INFINITY;
        const double ni2 = finite ? ni * ni : INFINITY;
        const double norm2 = nr2 + ni2;
        if (!finite || norm2 > p.bailout_sq) break;
        zr = nr;
        zi = ni;
        zr2 = nr2;
        zi2 = ni2;
    }

    d_colorize_escape_bgr(iter, p.iterations, p.colormap_id, out + 3 * idx);
}

template <int VariantId>
__global__ void ln_map_kernel_fp32_templated(CudaLnMapParams p, int row_start, int row_count, unsigned char* out) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    const int total = p.width_s * row_count;
    if (idx >= total) return;
    const int x = idx % p.width_s;
    const int local_row = idx / p.width_s;
    const int row = row_start + local_row;

    constexpr float tau = static_cast<float>(TAU);
    constexpr float ln_four = static_cast<float>(LN_FOUR);
    const float width_s = static_cast<float>(p.width_s);
    const float th = tau * static_cast<float>(x) / width_s;
    const float global_row = static_cast<float>(p.row_offset + static_cast<double>(row));
    const float k = ln_four - global_row * tau / width_s;
    const float r_mag = expf(k);
    const float pre = static_cast<float>(p.center_re) + r_mag * cosf(th);
    const float pim = static_cast<float>(p.center_im) + r_mag * sinf(th);

    float zr = p.julia ? pre : 0.0f;
    float zi = p.julia ? pim : 0.0f;
    const float cr = p.julia ? static_cast<float>(p.julia_re) : pre;
    const float ci = p.julia ? static_cast<float>(p.julia_im) : pim;
    float zr2 = zr * zr;
    float zi2 = zi * zi;
    const float bailout_sq = static_cast<float>(p.bailout_sq);

    int iter = 0;
    for (; iter < p.iterations; ++iter) {
        float nr = 0.0f, ni = 0.0f;
        d_step_cached_f<VariantId>(zr, zi, zr2, zi2, cr, ci, nr, ni);
        const bool finite = isfinite(nr) && isfinite(ni);
        const float nr2 = finite ? nr * nr : INFINITY;
        const float ni2 = finite ? ni * ni : INFINITY;
        const float norm2 = nr2 + ni2;
        if (!finite || norm2 > bailout_sq) break;
        zr = nr;
        zi = ni;
        zr2 = nr2;
        zi2 = ni2;
    }

    d_colorize_escape_bgr(iter, p.iterations, p.colormap_id, out + 3 * idx);
}

template <int VariantId>
__global__ void ln_map_kernel_fx64_templated(CudaLnMapParams p, int row_start, int row_count, unsigned char* out) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    const int total = p.width_s * row_count;
    if (idx >= total) return;
    const int x = idx % p.width_s;
    const int local_row = idx / p.width_s;
    const int row = row_start + local_row;

    const double th = TAU * static_cast<double>(x) / static_cast<double>(p.width_s);
    const double global_row = p.row_offset + static_cast<double>(row);
    const double k = LN_FOUR - global_row * TAU / static_cast<double>(p.width_s);
    const double r_mag = exp(k);
    const double pre = p.center_re + r_mag * cos(th);
    const double pim = p.center_im + r_mag * sin(th);

    using S = Fixed64<57>;
    S zr = p.julia ? S::from_double(pre) : S{0LL};
    S zi = p.julia ? S::from_double(pim) : S{0LL};
    const S cr = p.julia ? S::from_double(p.julia_re) : S::from_double(pre);
    const S ci = p.julia ? S::from_double(p.julia_im) : S::from_double(pim);
    uint64_t zr2_raw = fixed_square_q_sat_raw_cuda<57>(zr.raw);
    uint64_t zi2_raw = fixed_square_q_sat_raw_cuda<57>(zi.raw);
    S zr2{u64_to_i64_sat_cuda(zr2_raw)};
    S zi2{u64_to_i64_sat_cuda(zi2_raw)};
    const uint64_t bailout2_raw = fixed_positive_raw<57>(p.bailout_sq);

    int iter = 0;
    for (; iter < p.iterations; ++iter) {
        S nr, ni;
        d_step_fixed_cached<57, VariantId>(zr, zi, zr2, zi2, cr, ci, nr, ni);
        const uint64_t nr2_raw = fixed_square_q_sat_raw_cuda<57>(nr.raw);
        const uint64_t ni2_raw = fixed_square_q_sat_raw_cuda<57>(ni.raw);
        const uint64_t norm2 = add_u64_sat_cuda(nr2_raw, ni2_raw);
        if (norm2 > bailout2_raw) break;
        zr = nr;
        zi = ni;
        zr2 = S{u64_to_i64_sat_cuda(nr2_raw)};
        zi2 = S{u64_to_i64_sat_cuda(ni2_raw)};
    }

    d_colorize_escape_bgr(iter, p.iterations, p.colormap_id, out + 3 * idx);
}

// ---- Raw escape-count field kernels (write int counts, no colorize) ----
// Used by the mapped color modes (hist_eq, …) so they can run on the GPU at any
// precision instead of degrading to scalar OpenMP.
template <int VariantId>
__global__ void ln_map_iters_kernel_fp32(CudaLnMapParams p, int row_start, int row_count, int* out_iters) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    const int total = p.width_s * row_count;
    if (idx >= total) return;
    const int x = idx % p.width_s;
    const int row = row_start + idx / p.width_s;
    constexpr float tau = static_cast<float>(TAU);
    constexpr float ln_four = static_cast<float>(LN_FOUR);
    const float width_s = static_cast<float>(p.width_s);
    const float th = tau * static_cast<float>(x) / width_s;
    const float global_row = static_cast<float>(p.row_offset + static_cast<double>(row));
    const float k = ln_four - global_row * tau / width_s;
    const float r_mag = expf(k);
    const float pre = static_cast<float>(p.center_re) + r_mag * cosf(th);
    const float pim = static_cast<float>(p.center_im) + r_mag * sinf(th);
    float zr = p.julia ? pre : 0.0f;
    float zi = p.julia ? pim : 0.0f;
    const float cr = p.julia ? static_cast<float>(p.julia_re) : pre;
    const float ci = p.julia ? static_cast<float>(p.julia_im) : pim;
    float zr2 = zr * zr, zi2 = zi * zi;
    const float bailout_sq = static_cast<float>(p.bailout_sq);
    int iter = 0;
    for (; iter < p.iterations; ++iter) {
        float nr = 0.0f, ni = 0.0f;
        d_step_cached_f<VariantId>(zr, zi, zr2, zi2, cr, ci, nr, ni);
        const bool finite = isfinite(nr) && isfinite(ni);
        const float nr2 = finite ? nr * nr : INFINITY;
        const float ni2 = finite ? ni * ni : INFINITY;
        if (!finite || nr2 + ni2 > bailout_sq) break;
        zr = nr; zi = ni; zr2 = nr2; zi2 = ni2;
    }
    out_iters[idx] = iter;
}

template <int VariantId>
__global__ void ln_map_iters_kernel(CudaLnMapParams p, int row_start, int row_count, int* out_iters) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    const int total = p.width_s * row_count;
    if (idx >= total) return;
    const int x = idx % p.width_s;
    const int row = row_start + idx / p.width_s;
    const double th = TAU * static_cast<double>(x) / static_cast<double>(p.width_s);
    const double global_row = p.row_offset + static_cast<double>(row);
    const double k = LN_FOUR - global_row * TAU / static_cast<double>(p.width_s);
    const double r_mag = exp(k);
    const double pre = p.center_re + r_mag * cos(th);
    const double pim = p.center_im + r_mag * sin(th);
    double zr = p.julia ? pre : 0.0;
    double zi = p.julia ? pim : 0.0;
    const double cr = p.julia ? p.julia_re : pre;
    const double ci = p.julia ? p.julia_im : pim;
    double zr2 = zr * zr, zi2 = zi * zi;
    int iter = 0;
    for (; iter < p.iterations; ++iter) {
        double nr = 0.0, ni = 0.0;
        d_step_cached<VariantId>(zr, zi, zr2, zi2, cr, ci, nr, ni);
        const bool finite = isfinite(nr) && isfinite(ni);
        const double nr2 = finite ? nr * nr : INFINITY;
        const double ni2 = finite ? ni * ni : INFINITY;
        if (!finite || nr2 + ni2 > p.bailout_sq) break;
        zr = nr; zi = ni; zr2 = nr2; zi2 = ni2;
    }
    out_iters[idx] = iter;
}

template <int VariantId>
__global__ void ln_map_iters_kernel_fx64(CudaLnMapParams p, int row_start, int row_count, int* out_iters) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    const int total = p.width_s * row_count;
    if (idx >= total) return;
    const int x = idx % p.width_s;
    const int row = row_start + idx / p.width_s;
    const double th = TAU * static_cast<double>(x) / static_cast<double>(p.width_s);
    const double global_row = p.row_offset + static_cast<double>(row);
    const double k = LN_FOUR - global_row * TAU / static_cast<double>(p.width_s);
    const double r_mag = exp(k);
    const double pre = p.center_re + r_mag * cos(th);
    const double pim = p.center_im + r_mag * sin(th);
    using S = Fixed64<57>;
    S zr = p.julia ? S::from_double(pre) : S{0LL};
    S zi = p.julia ? S::from_double(pim) : S{0LL};
    const S cr = p.julia ? S::from_double(p.julia_re) : S::from_double(pre);
    const S ci = p.julia ? S::from_double(p.julia_im) : S::from_double(pim);
    S zr2{u64_to_i64_sat_cuda(fixed_square_q_sat_raw_cuda<57>(zr.raw))};
    S zi2{u64_to_i64_sat_cuda(fixed_square_q_sat_raw_cuda<57>(zi.raw))};
    const uint64_t bailout2_raw = fixed_positive_raw<57>(p.bailout_sq);
    int iter = 0;
    for (; iter < p.iterations; ++iter) {
        S nr, ni;
        d_step_fixed_cached<57, VariantId>(zr, zi, zr2, zi2, cr, ci, nr, ni);
        const uint64_t nr2_raw = fixed_square_q_sat_raw_cuda<57>(nr.raw);
        const uint64_t ni2_raw = fixed_square_q_sat_raw_cuda<57>(ni.raw);
        if (add_u64_sat_cuda(nr2_raw, ni2_raw) > bailout2_raw) break;
        zr = nr; zi = ni;
        zr2 = S{u64_to_i64_sat_cuda(nr2_raw)};
        zi2 = S{u64_to_i64_sat_cuda(ni2_raw)};
    }
    out_iters[idx] = iter;
}

template <int VariantId>
void launch_ln_map_iters_variant(const CudaLnMapParams& p, int row_start, int row_count, int* d_iters, bool fx64) {
    const int block = 256;
    const int grid = (p.width_s * row_count + block - 1) / block;
    if (fx64) ln_map_iters_kernel_fx64<VariantId><<<grid, block>>>(p, row_start, row_count, d_iters);
    else      ln_map_iters_kernel<VariantId><<<grid, block>>>(p, row_start, row_count, d_iters);
}

void launch_ln_map_iters(const CudaLnMapParams& p, int row_start, int row_count, int* d_iters, bool fx64) {
    switch (p.variant_id) {
        case 0: launch_ln_map_iters_variant<0>(p, row_start, row_count, d_iters, fx64); break;
        case 1: launch_ln_map_iters_variant<1>(p, row_start, row_count, d_iters, fx64); break;
        case 2: launch_ln_map_iters_variant<2>(p, row_start, row_count, d_iters, fx64); break;
        case 3: launch_ln_map_iters_variant<3>(p, row_start, row_count, d_iters, fx64); break;
        case 4: launch_ln_map_iters_variant<4>(p, row_start, row_count, d_iters, fx64); break;
        case 5: launch_ln_map_iters_variant<5>(p, row_start, row_count, d_iters, fx64); break;
        case 6: launch_ln_map_iters_variant<6>(p, row_start, row_count, d_iters, fx64); break;
        case 7: launch_ln_map_iters_variant<7>(p, row_start, row_count, d_iters, fx64); break;
        case 8: launch_ln_map_iters_variant<8>(p, row_start, row_count, d_iters, fx64); break;
        case 9: launch_ln_map_iters_variant<9>(p, row_start, row_count, d_iters, fx64); break;
        default: throw std::runtime_error("CUDA ln-map unsupported variant");
    }
}

template <int VariantId>
void launch_ln_map_iters_fp32_variant(const CudaLnMapParams& p, int row_start, int row_count, int* d_iters) {
    const int block = 256;
    const int grid = (p.width_s * row_count + block - 1) / block;
    ln_map_iters_kernel_fp32<VariantId><<<grid, block>>>(p, row_start, row_count, d_iters);
}

void launch_ln_map_iters_fp32(const CudaLnMapParams& p, int row_start, int row_count, int* d_iters) {
    switch (p.variant_id) {
        case 0: launch_ln_map_iters_fp32_variant<0>(p, row_start, row_count, d_iters); break;
        case 1: launch_ln_map_iters_fp32_variant<1>(p, row_start, row_count, d_iters); break;
        case 2: launch_ln_map_iters_fp32_variant<2>(p, row_start, row_count, d_iters); break;
        case 3: launch_ln_map_iters_fp32_variant<3>(p, row_start, row_count, d_iters); break;
        case 4: launch_ln_map_iters_fp32_variant<4>(p, row_start, row_count, d_iters); break;
        case 5: launch_ln_map_iters_fp32_variant<5>(p, row_start, row_count, d_iters); break;
        case 6: launch_ln_map_iters_fp32_variant<6>(p, row_start, row_count, d_iters); break;
        case 7: launch_ln_map_iters_fp32_variant<7>(p, row_start, row_count, d_iters); break;
        case 8: launch_ln_map_iters_fp32_variant<8>(p, row_start, row_count, d_iters); break;
        case 9: launch_ln_map_iters_fp32_variant<9>(p, row_start, row_count, d_iters); break;
        default: throw std::runtime_error("CUDA ln-map unsupported variant");
    }
}

template <int VariantId>
void launch_ln_map_variant(const CudaLnMapParams& p, int row_start, int row_count, unsigned char* d_out) {
    const int block = 256;
    const int total = p.width_s * row_count;
    const int grid = (total + block - 1) / block;
    ln_map_kernel_templated<VariantId><<<grid, block>>>(p, row_start, row_count, d_out);
}

template <int VariantId>
void launch_ln_map_fp32_variant(const CudaLnMapParams& p, int row_start, int row_count, unsigned char* d_out) {
    const int block = 256;
    const int total = p.width_s * row_count;
    const int grid = (total + block - 1) / block;
    ln_map_kernel_fp32_templated<VariantId><<<grid, block>>>(p, row_start, row_count, d_out);
}

template <int VariantId>
void launch_ln_map_fx64_variant(const CudaLnMapParams& p, int row_start, int row_count, unsigned char* d_out) {
    const int block = 256;
    const int total = p.width_s * row_count;
    const int grid = (total + block - 1) / block;
    ln_map_kernel_fx64_templated<VariantId><<<grid, block>>>(p, row_start, row_count, d_out);
}

void launch_ln_map(const CudaLnMapParams& p, int row_start, int row_count, unsigned char* d_out) {
    switch (p.variant_id) {
        case 0: launch_ln_map_variant<0>(p, row_start, row_count, d_out); break;
        case 1: launch_ln_map_variant<1>(p, row_start, row_count, d_out); break;
        case 2: launch_ln_map_variant<2>(p, row_start, row_count, d_out); break;
        case 3: launch_ln_map_variant<3>(p, row_start, row_count, d_out); break;
        case 4: launch_ln_map_variant<4>(p, row_start, row_count, d_out); break;
        case 5: launch_ln_map_variant<5>(p, row_start, row_count, d_out); break;
        case 6: launch_ln_map_variant<6>(p, row_start, row_count, d_out); break;
        case 7: launch_ln_map_variant<7>(p, row_start, row_count, d_out); break;
        case 8: launch_ln_map_variant<8>(p, row_start, row_count, d_out); break;
        case 9: launch_ln_map_variant<9>(p, row_start, row_count, d_out); break;
        default: throw std::runtime_error("CUDA ln-map unsupported variant");
    }
}

void launch_ln_map_fp32(const CudaLnMapParams& p, int row_start, int row_count, unsigned char* d_out) {
    switch (p.variant_id) {
        case 0: launch_ln_map_fp32_variant<0>(p, row_start, row_count, d_out); break;
        case 1: launch_ln_map_fp32_variant<1>(p, row_start, row_count, d_out); break;
        case 2: launch_ln_map_fp32_variant<2>(p, row_start, row_count, d_out); break;
        case 3: launch_ln_map_fp32_variant<3>(p, row_start, row_count, d_out); break;
        case 4: launch_ln_map_fp32_variant<4>(p, row_start, row_count, d_out); break;
        case 5: launch_ln_map_fp32_variant<5>(p, row_start, row_count, d_out); break;
        case 6: launch_ln_map_fp32_variant<6>(p, row_start, row_count, d_out); break;
        case 7: launch_ln_map_fp32_variant<7>(p, row_start, row_count, d_out); break;
        case 8: launch_ln_map_fp32_variant<8>(p, row_start, row_count, d_out); break;
        case 9: launch_ln_map_fp32_variant<9>(p, row_start, row_count, d_out); break;
        default: throw std::runtime_error("CUDA ln-map unsupported variant");
    }
}

void launch_ln_map_fx64(const CudaLnMapParams& p, int row_start, int row_count, unsigned char* d_out) {
    switch (p.variant_id) {
        case 0: launch_ln_map_fx64_variant<0>(p, row_start, row_count, d_out); break;
        case 1: launch_ln_map_fx64_variant<1>(p, row_start, row_count, d_out); break;
        case 2: launch_ln_map_fx64_variant<2>(p, row_start, row_count, d_out); break;
        case 3: launch_ln_map_fx64_variant<3>(p, row_start, row_count, d_out); break;
        case 4: launch_ln_map_fx64_variant<4>(p, row_start, row_count, d_out); break;
        case 5: launch_ln_map_fx64_variant<5>(p, row_start, row_count, d_out); break;
        case 6: launch_ln_map_fx64_variant<6>(p, row_start, row_count, d_out); break;
        case 7: launch_ln_map_fx64_variant<7>(p, row_start, row_count, d_out); break;
        case 8: launch_ln_map_fx64_variant<8>(p, row_start, row_count, d_out); break;
        case 9: launch_ln_map_fx64_variant<9>(p, row_start, row_count, d_out); break;
        default: throw std::runtime_error("CUDA ln-map unsupported variant");
    }
}

void ensure_out(const CudaLnMapParams& p, cv::Mat& out) {
    if (out.empty() || out.rows != p.height_t || out.cols != p.width_s || out.type() != CV_8UC3) {
        out.create(p.height_t, p.width_s, CV_8UC3);
    }
}

using LnMapLaunchFn = void (*)(const CudaLnMapParams&, int, int, unsigned char*);

CudaLnMapStats cuda_render_ln_map_rows_impl(
    const CudaLnMapParams& p,
    cv::Mat& out,
    int row_start,
    int row_count,
    LnMapLaunchFn launch
) {
    if (!cuda_ln_map_available()) throw std::runtime_error("CUDA ln-map not available");
    if (p.variant_id < 0 || p.variant_id > 9) throw std::runtime_error("CUDA ln-map unsupported variant");
    if (row_start < 0 || row_count <= 0 || row_start + row_count > p.height_t) {
        throw std::runtime_error("invalid CUDA ln-map row range");
    }
    std::lock_guard<std::mutex> lock(g_ln_mu);
    ensure_out(p, out);

    const size_t row_bytes = static_cast<size_t>(p.width_s) * 3u;
    const size_t bytes = row_bytes * static_cast<size_t>(row_count);
    unsigned char* d_out = nullptr;
    ScopedDevicePtr transient_out;
    if (bytes <= CACHED_LN_DEVBUF_MAX_BYTES) {
        g_ln_devbuf.ensure(bytes);
        d_out = g_ln_devbuf.d;
    } else {
        CUDA_LN_CHECK(cudaMalloc(reinterpret_cast<void**>(&transient_out.ptr), bytes));
        d_out = transient_out.ptr;
    }

    g_ln_events.ensure();
    CUDA_LN_CHECK(cudaEventRecord(g_ln_events.start));
    launch(p, row_start, row_count, d_out);
    CUDA_LN_CHECK(cudaGetLastError());
    CUDA_LN_CHECK(cudaEventRecord(g_ln_events.stop));
    CUDA_LN_CHECK(cudaEventSynchronize(g_ln_events.stop));

    float ms = 0.0f;
    CUDA_LN_CHECK(cudaEventElapsedTime(&ms, g_ln_events.start, g_ln_events.stop));
    if (out.isContinuous() && out.step == row_bytes) {
        CUDA_LN_CHECK(cudaMemcpy(out.ptr<unsigned char>(row_start), d_out, bytes, cudaMemcpyDeviceToHost));
    } else {
        std::vector<unsigned char> tmp(bytes);
        CUDA_LN_CHECK(cudaMemcpy(tmp.data(), d_out, bytes, cudaMemcpyDeviceToHost));
        for (int r = 0; r < row_count; ++r) {
            std::memcpy(out.ptr<unsigned char>(row_start + r), tmp.data() + static_cast<size_t>(r) * row_bytes, row_bytes);
        }
    }

    CudaLnMapStats stats;
    stats.elapsed_ms = static_cast<double>(ms);
    return stats;
}

} // namespace

bool cuda_ln_map_available() noexcept {
    int count = 0;
    return cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

CudaLnMapStats cuda_render_ln_map_rows(const CudaLnMapParams& p, cv::Mat& out, int row_start, int row_count) {
    return cuda_render_ln_map_rows_impl(p, out, row_start, row_count, launch_ln_map);
}

// Raw escape-count field on the GPU (fp64 or fx64), for the mapped color modes.
CudaLnMapStats cuda_render_ln_map_iters_rows(const CudaLnMapParams& p, int* iters, int row_start, int row_count, bool fx64) {
    if (!cuda_ln_map_available()) throw std::runtime_error("CUDA ln-map not available");
    if (p.variant_id < 0 || p.variant_id > 9) throw std::runtime_error("CUDA ln-map unsupported variant");
    if (row_start < 0 || row_count <= 0 || row_start + row_count > p.height_t) {
        throw std::runtime_error("invalid CUDA ln-map row range");
    }
    std::lock_guard<std::mutex> lock(g_ln_mu);
    const size_t count = static_cast<size_t>(p.width_s) * static_cast<size_t>(row_count);
    const size_t bytes = count * sizeof(int);
    ScopedDevicePtr transient;
    CUDA_LN_CHECK(cudaMalloc(reinterpret_cast<void**>(&transient.ptr), bytes));
    int* d_iters = reinterpret_cast<int*>(transient.ptr);

    g_ln_events.ensure();
    CUDA_LN_CHECK(cudaEventRecord(g_ln_events.start));
    launch_ln_map_iters(p, row_start, row_count, d_iters, fx64);
    CUDA_LN_CHECK(cudaGetLastError());
    CUDA_LN_CHECK(cudaEventRecord(g_ln_events.stop));
    CUDA_LN_CHECK(cudaEventSynchronize(g_ln_events.stop));
    float ms = 0.0f;
    CUDA_LN_CHECK(cudaEventElapsedTime(&ms, g_ln_events.start, g_ln_events.stop));
    CUDA_LN_CHECK(cudaMemcpy(iters + static_cast<size_t>(row_start) * static_cast<size_t>(p.width_s),
                             d_iters, bytes, cudaMemcpyDeviceToHost));
    CudaLnMapStats stats;
    stats.elapsed_ms = static_cast<double>(ms);
    return stats;
}

CudaLnMapStats cuda_render_ln_map(const CudaLnMapParams& p, cv::Mat& out) {
    return cuda_render_ln_map_rows(p, out, 0, p.height_t);
}

CudaLnMapStats cuda_render_ln_map_iters_fp32_rows(const CudaLnMapParams& p, int* iters, int row_start, int row_count) {
    if (!cuda_ln_map_available()) throw std::runtime_error("CUDA ln-map not available");
    if (p.variant_id < 0 || p.variant_id > 9) throw std::runtime_error("CUDA ln-map unsupported variant");
    if (row_start < 0 || row_count <= 0 || row_start + row_count > p.height_t) {
        throw std::runtime_error("invalid CUDA ln-map row range");
    }
    std::lock_guard<std::mutex> lock(g_ln_mu);
    const size_t count = static_cast<size_t>(p.width_s) * static_cast<size_t>(row_count);
    const size_t bytes = count * sizeof(int);
    ScopedDevicePtr transient;
    CUDA_LN_CHECK(cudaMalloc(reinterpret_cast<void**>(&transient.ptr), bytes));
    int* d_iters = reinterpret_cast<int*>(transient.ptr);

    g_ln_events.ensure();
    CUDA_LN_CHECK(cudaEventRecord(g_ln_events.start));
    launch_ln_map_iters_fp32(p, row_start, row_count, d_iters);
    CUDA_LN_CHECK(cudaGetLastError());
    CUDA_LN_CHECK(cudaEventRecord(g_ln_events.stop));
    CUDA_LN_CHECK(cudaEventSynchronize(g_ln_events.stop));
    float ms = 0.0f;
    CUDA_LN_CHECK(cudaEventElapsedTime(&ms, g_ln_events.start, g_ln_events.stop));
    CUDA_LN_CHECK(cudaMemcpy(iters + static_cast<size_t>(row_start) * static_cast<size_t>(p.width_s),
                             d_iters, bytes, cudaMemcpyDeviceToHost));
    CudaLnMapStats stats;
    stats.elapsed_ms = static_cast<double>(ms);
    return stats;
}

CudaLnMapStats cuda_render_ln_map_fp32_rows(const CudaLnMapParams& p, cv::Mat& out, int row_start, int row_count) {
    return cuda_render_ln_map_rows_impl(p, out, row_start, row_count, launch_ln_map_fp32);
}

CudaLnMapStats cuda_render_ln_map_fp32(const CudaLnMapParams& p, cv::Mat& out) {
    return cuda_render_ln_map_fp32_rows(p, out, 0, p.height_t);
}

CudaLnMapStats cuda_render_ln_map_fx64_rows(const CudaLnMapParams& p, cv::Mat& out, int row_start, int row_count) {
    return cuda_render_ln_map_rows_impl(p, out, row_start, row_count, launch_ln_map_fx64);
}

CudaLnMapStats cuda_render_ln_map_fx64(const CudaLnMapParams& p, cv::Mat& out) {
    return cuda_render_ln_map_fx64_rows(p, out, 0, p.height_t);
}

} // namespace fsd_cuda
