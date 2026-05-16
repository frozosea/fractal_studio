// compute/cuda/map_kernel.cu
//
// CUDA fractal renderer supporting all 10 variants, Julia mode,
// and non-escape metrics (MinAbs, MaxAbs, Envelope).
//
// Design:
//   - One kernel handles all pixels via a flat thread grid (pixel = thread).
//   - Variant and metric are selected at host-call time and compiled into
//     templated fp32/fp64/fx64 kernels.
//   - Output: raw BGR byte array copied into a cv::Mat on the host.
//
// Thread layout: 32×8 blocks. Each warp covers a contiguous row segment,
// improving row-major stores while keeping 256 threads per block.

#include "map_kernel.cuh"
#include "fx64.cuh"

#include <opencv2/core.hpp>

#include <cuda_runtime.h>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <string>

#define CUDA_CHECK(expr)  do {                                                \
    cudaError_t _e = (expr);                                                  \
    if (_e != cudaSuccess)                                                    \
        throw std::runtime_error(std::string("CUDA: ") + cudaGetErrorString(_e) + \
            " at " __FILE__ ":" + std::to_string(__LINE__));                  \
} while(0)

namespace fsd_cuda {

// ---- Device colormap helpers (pixel-exact match with colormap.hpp) ----

__device__ inline int d_clamp255(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

__device__ inline float d_cos_color(float n, float freq) {
    constexpr float PI = 3.14159265f;
    return 128.0f - 128.0f * cosf(freq * n * PI);
}

__device__ inline void d_hsv_to_rgb(float h, float s, float v,
                                     int& r, int& g, int& b) {
    const float c  = v * s;
    const float hh = h / 60.0f;
    const float x  = c * (1.0f - fabsf(fmodf(hh, 2.0f) - 1.0f));

    float rr = 0.0f, gg = 0.0f, bb = 0.0f;
    if      (hh < 1.0f) { rr = c; gg = x; }
    else if (hh < 2.0f) { rr = x; gg = c; }
    else if (hh < 3.0f) { gg = c; bb = x; }
    else if (hh < 4.0f) { gg = x; bb = c; }
    else if (hh < 5.0f) { rr = x; bb = c; }
    else                { rr = c; bb = x; }

    const float m = v - c;
    r = d_clamp255(static_cast<int>((rr + m) * 255.0f));
    g = d_clamp255(static_cast<int>((gg + m) * 255.0f));
    b = d_clamp255(static_cast<int>((bb + m) * 255.0f));
}

// Colorize one escaped pixel.  Matches colormap.hpp pixel-exact.
// colormap_id: 0=ClassicCos, 1=Mod17, 2=HsvWheel, 3=Tri765, 4=Grayscale
// Writes BGR into px[0..2].
__device__ inline void colorize_escape_bgr(int iter, int max_iter,
                                            int colormap_id, uint8_t* px) {
    if (iter >= max_iter) {
        px[0] = px[1] = px[2] = 255;   // interior: white
        return;
    }
    // n matches CPU: (iter+1)/(max_iter+2)
    const float n = (static_cast<float>(iter) + 1.0f) /
                    (static_cast<float>(max_iter) + 2.0f);

    switch (colormap_id) {
        case 1: {  // Mod17
            px[2] = static_cast<uint8_t>(d_clamp255(iter % 256));
            px[1] = static_cast<uint8_t>(d_clamp255(iter / 256));
            px[0] = static_cast<uint8_t>(d_clamp255((iter % 17) * 17));
            return;
        }
        case 2: {  // HsvWheel
            const float h = fmodf(static_cast<float>(iter), 1440.0f) / 4.0f;
            int r = 0, g = 0, b = 0;
            d_hsv_to_rgb(h, 1.0f, 1.0f, r, g, b);
            px[2] = static_cast<uint8_t>(r);
            px[1] = static_cast<uint8_t>(g);
            px[0] = static_cast<uint8_t>(b);
            return;
        }
        case 3: {  // Tri765
            const int m    = iter % 765;
            const int band = m / 255;
            const int d    = m % 255;
            int rr = 255, gg = 255, bb = 255;
            if      (band == 0) { rr = 255 - d; gg = d;       bb = 255;     }
            else if (band == 1) { rr = d;       gg = 255;     bb = 255 - d; }
            else                { rr = 255;     gg = 255 - d; bb = d;       }
            px[2] = static_cast<uint8_t>(d_clamp255(rr));
            px[1] = static_cast<uint8_t>(d_clamp255(gg));
            px[0] = static_cast<uint8_t>(d_clamp255(bb));
            return;
        }
        case 4: {  // Grayscale
            const uint8_t v = static_cast<uint8_t>(d_clamp255(static_cast<int>(n * 255.0f)));
            px[0] = px[1] = px[2] = v;
            return;
        }
        case 5:    // HsRainbow is HS raw-field only; escape matches CPU ClassicCos fallback.
        default:  // 0 = ClassicCos (and any unknown id)
            px[2] = static_cast<uint8_t>(d_clamp255(static_cast<int>(d_cos_color(n,  53.0f))));
            px[1] = static_cast<uint8_t>(d_clamp255(static_cast<int>(d_cos_color(n,  27.0f))));
            px[0] = static_cast<uint8_t>(d_clamp255(static_cast<int>(d_cos_color(n, 139.0f))));
            return;
    }
}

__device__ inline void d_hsv_to_rgb_d(double h, double s, double v,
                                      int& r, int& g, int& b) {
    const double c  = v * s;
    const double hh = h / 60.0;
    const double x  = c * (1.0 - fabs(fmod(hh, 2.0) - 1.0));

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

__device__ inline void d_rainbow_from_index(int idx, uint8_t* px) {
    if (idx <= 0) {
        px[0] = px[1] = px[2] = 0;
        return;
    }
    if (idx >= 1785) {
        px[0] = px[1] = px[2] = 255;
        return;
    }

    int a0 = idx, a1 = 0, a2 = 0;
    if      (255 < a0 && a0 < 510)  { a1 = a0 - 255; a0 = 510 - a0; }
    else if (509 < a0 && a0 < 765)  { a1 = 255; a0 = a0 - 510; }
    else if (764 < a0 && a0 < 1020) { a2 = a0 - 765; a1 = 1020 - a0; a0 = a1; }
    else if (1019 < a0 && a0 < 1275){ a2 = 255; a0 = a0 - 1020; }
    else if (1274 < a0 && a0 < 1530){ a2 = 255; a1 = a0 - 1275; a0 = 1530 - a0; }
    else if (a0 > 1529)             { a2 = 255; a1 = 255; a0 = a0 - 1530; }

    px[2] = static_cast<uint8_t>(d_clamp255(a1));
    px[1] = static_cast<uint8_t>(d_clamp255(a2));
    px[0] = static_cast<uint8_t>(d_clamp255(a0));
}

__device__ inline void colorize_field_hs_bgr(double x, uint8_t* px) {
    if (x <= 0.0 || !isfinite(x)) {
        px[0] = px[1] = px[2] = 255;
        return;
    }
    const double raw = (36.0 / 35.0 - log2(x)) * 35.0;
    int idx = static_cast<int>(raw);
    if (idx < 0) idx = 0;
    if (idx > 1785) idx = 1785;
    d_rainbow_from_index(idx, px);
}

__device__ inline void colorize_field_bgr(double v01, int colormap_id, uint8_t* px) {
    if (v01 < 0.0) v01 = 0.0;
    if (v01 > 1.0) v01 = 1.0;
    constexpr double PI = 3.141592653589793;

    switch (colormap_id) {
        case 4: {
            const uint8_t v = static_cast<uint8_t>(d_clamp255(static_cast<int>(v01 * 255.0)));
            px[0] = px[1] = px[2] = v;
            return;
        }
        case 2: {
            int r = 0, g = 0, b = 0;
            d_hsv_to_rgb_d(v01 * 360.0, 1.0, 1.0, r, g, b);
            px[2] = static_cast<uint8_t>(r);
            px[1] = static_cast<uint8_t>(g);
            px[0] = static_cast<uint8_t>(b);
            return;
        }
        case 3: {
            const int m    = static_cast<int>(v01 * 765.0);
            const int band = (m / 255) % 3;
            const int d    = m % 255;
            int rr = 255, gg = 255, bb = 255;
            if      (band == 0) { rr = 255 - d; gg = d;       bb = 255;     }
            else if (band == 1) { rr = d;       gg = 255;     bb = 255 - d; }
            else                { rr = 255;     gg = 255 - d; bb = d;       }
            px[2] = static_cast<uint8_t>(d_clamp255(rr));
            px[1] = static_cast<uint8_t>(d_clamp255(gg));
            px[0] = static_cast<uint8_t>(d_clamp255(bb));
            return;
        }
        case 1: {
            int idx = static_cast<int>(v01 * 17.0);
            if (idx > 16) idx = 16;
            const uint8_t v = static_cast<uint8_t>(idx * 15);
            px[0] = px[1] = px[2] = v;
            return;
        }
        case 5: {
            int idx = static_cast<int>(v01 * 1785.0);
            if (idx > 1785) idx = 1785;
            d_rainbow_from_index(idx, px);
            return;
        }
        case 0:
        default:
            px[2] = static_cast<uint8_t>(d_clamp255(static_cast<int>(128.0 - 128.0 * cos(v01 * 2.0 * PI))));
            px[1] = static_cast<uint8_t>(d_clamp255(static_cast<int>(128.0 - 128.0 * cos(v01 * 2.0 * PI + 2.094395))));
            px[0] = static_cast<uint8_t>(d_clamp255(static_cast<int>(128.0 - 128.0 * cos(v01 * 2.0 * PI + 4.188790))));
            return;
    }
}

__device__ inline void colorize_metric_field_bgr(double raw, double v01, int colormap_id, uint8_t* px) {
    if (colormap_id == 5) {
        colorize_field_hs_bgr(raw, px);
        return;
    }
    colorize_field_bgr(v01, colormap_id, px);
}

template <int VariantId>
__device__ inline void step_fp64_cached(
    double zre,
    double zim,
    double zre2,
    double zim2,
    double cre,
    double cim,
    double& new_re,
    double& new_im
) {
    const double sq_re = zre2 - zim2;
    if constexpr (VariantId == 1) {
        const double sq_im = 2.0 * zre * zim;
        new_re = sq_re + cre;
        new_im = -sq_im + cim;
    } else if constexpr (VariantId == 2) {
        const double sq_im = 2.0 * fabs(zre) * fabs(zim);
        new_re = sq_re + cre;
        new_im = sq_im + cim;
    } else if constexpr (VariantId == 3) {
        const double sq_im = 2.0 * zre * fabs(zim);
        new_re = sq_re + cre;
        new_im = sq_im + cim;
    } else if constexpr (VariantId == 4) {
        const double sq_im = 2.0 * fabs(zre) * (-zim);
        new_re = sq_re + cre;
        new_im = sq_im + cim;
    } else if constexpr (VariantId == 5) {
        const double sq_im = 2.0 * zre * zim;
        new_re = fabs(sq_re) + cre;
        new_im = sq_im + cim;
    } else if constexpr (VariantId == 6) {
        const double sq_im = 2.0 * zre * zim;
        new_re = fabs(sq_re) + cre;
        new_im = -sq_im + cim;
    } else if constexpr (VariantId == 7) {
        const double sq_im = 2.0 * zre * zim;
        new_re = fabs(sq_re) + cre;
        new_im = fabs(sq_im) + cim;
    } else if constexpr (VariantId == 8) {
        const double sq_im = 2.0 * zre * fabs(zim);
        new_re = fabs(sq_re) + cre;
        new_im = sq_im + cim;
    } else if constexpr (VariantId == 9) {
        const double sq_im = 2.0 * fabs(zre) * zim;
        new_re = fabs(sq_re) + cre;
        new_im = -sq_im + cim;
    } else {
        const double sq_im = 2.0 * zre * zim;
        new_re = sq_re + cre;
        new_im = sq_im + cim;
    }
}

template <int VariantId>
__device__ inline void step_fp32_cached(
    float zre,
    float zim,
    float zre2,
    float zim2,
    float cre,
    float cim,
    float& new_re,
    float& new_im
) {
    const float sq_re = zre2 - zim2;
    if constexpr (VariantId == 1) {
        const float sq_im = 2.0f * zre * zim;
        new_re = sq_re + cre;
        new_im = -sq_im + cim;
    } else if constexpr (VariantId == 2) {
        const float sq_im = 2.0f * fabsf(zre) * fabsf(zim);
        new_re = sq_re + cre;
        new_im = sq_im + cim;
    } else if constexpr (VariantId == 3) {
        const float sq_im = 2.0f * zre * fabsf(zim);
        new_re = sq_re + cre;
        new_im = sq_im + cim;
    } else if constexpr (VariantId == 4) {
        const float sq_im = 2.0f * fabsf(zre) * (-zim);
        new_re = sq_re + cre;
        new_im = sq_im + cim;
    } else if constexpr (VariantId == 5) {
        const float sq_im = 2.0f * zre * zim;
        new_re = fabsf(sq_re) + cre;
        new_im = sq_im + cim;
    } else if constexpr (VariantId == 6) {
        const float sq_im = 2.0f * zre * zim;
        new_re = fabsf(sq_re) + cre;
        new_im = -sq_im + cim;
    } else if constexpr (VariantId == 7) {
        const float sq_im = 2.0f * zre * zim;
        new_re = fabsf(sq_re) + cre;
        new_im = fabsf(sq_im) + cim;
    } else if constexpr (VariantId == 8) {
        const float sq_im = 2.0f * zre * fabsf(zim);
        new_re = fabsf(sq_re) + cre;
        new_im = sq_im + cim;
    } else if constexpr (VariantId == 9) {
        const float sq_im = 2.0f * fabsf(zre) * zim;
        new_re = fabsf(sq_re) + cre;
        new_im = -sq_im + cim;
    } else {
        const float sq_im = 2.0f * zre * zim;
        new_re = sq_re + cre;
        new_im = sq_im + cim;
    }
}

// ---- fp32 kernel ----

template <int VariantId, int MetricId>
__global__ void fractal_fp32(
    float center_re, float center_im, float scale,
    int W, int H, int max_iter, float bail2, int colormap_id,
    bool julia, float julia_re_p, float julia_im_p,
    uint8_t* __restrict__ out
) {
    const int px_x = blockIdx.x * blockDim.x + threadIdx.x;
    const int px_y = blockIdx.y * blockDim.y + threadIdx.y;
    if (px_x >= W || px_y >= H) return;

    const float aspect = static_cast<float>(W) / static_cast<float>(H);
    const float span_im = scale;
    const float span_re = scale * aspect;
    const float re = (center_re - span_re * 0.5f) +
        (static_cast<float>(px_x) + 0.5f) / static_cast<float>(W) * span_re;
    const float im = (center_im + span_im * 0.5f) -
        (static_cast<float>(px_y) + 0.5f) / static_cast<float>(H) * span_im;

    float zre, zim, cre, cim;
    if (julia) {
        zre = re;   zim = im;
        cre = julia_re_p; cim = julia_im_p;
    } else {
        zre = 0.0f; zim = 0.0f;
        cre = re;   cim = im;
    }

    constexpr bool track_min = (MetricId == 1 || MetricId == 3);
    constexpr bool track_max = (MetricId == 2 || MetricId == 3);
    float mn = INFINITY;
    float mx = 0.0f;
    float zre2 = zre * zre;
    float zim2 = zim * zim;

    int i = 0;
    for (; i < max_iter; i++) {
        float next_re = 0.0f;
        float next_im = 0.0f;
        step_fp32_cached<VariantId>(zre, zim, zre2, zim2, cre, cim, next_re, next_im);
        const bool finite_z = isfinite(next_re) && isfinite(next_im);
        float next_re2 = 0.0f;
        float next_im2 = 0.0f;
        const float abs2 = finite_z
            ? ((next_re2 = next_re * next_re) + (next_im2 = next_im * next_im))
            : INFINITY;
        if constexpr (track_min) {
            if (abs2 < mn) mn = abs2;
        }
        if constexpr (track_max) {
            if (abs2 > mx) mx = abs2;
        }
        if (!finite_z || abs2 > bail2) break;
        zre = next_re;
        zim = next_im;
        zre2 = next_re2;
        zim2 = next_im2;
    }

    uint8_t* px = out + (static_cast<size_t>(px_y) * W + px_x) * 3;

    if constexpr (MetricId == 1) {
        const double bailout = sqrt(static_cast<double>(bail2));
        const double raw = sqrt(static_cast<double>(mn));
        const double v01 = fmin(1.0, raw / bailout);
        colorize_metric_field_bgr(raw, v01, colormap_id, px);
    } else if constexpr (MetricId == 2) {
        const double bailout = sqrt(static_cast<double>(bail2));
        const double raw = sqrt(static_cast<double>(mx));
        const double v01 = fmin(1.0, raw / bailout);
        colorize_metric_field_bgr(raw, v01, colormap_id, px);
    } else if constexpr (MetricId == 3) {
        const double bailout = sqrt(static_cast<double>(bail2));
        const double raw = 0.5 * (sqrt(static_cast<double>(mn)) + sqrt(static_cast<double>(mx)));
        const double v01 = fmin(1.0, raw / bailout);
        colorize_metric_field_bgr(raw, v01, colormap_id, px);
    } else {
        colorize_escape_bgr(i, max_iter, colormap_id, px);
    }
}

// ---- fp64 kernel ----

template <int VariantId, int MetricId>
__global__ void fractal_fp64(
    double center_re, double center_im, double scale,
    int W, int H, int max_iter, double bail2, int colormap_id,
    bool julia, double julia_re_p, double julia_im_p,
    uint8_t* __restrict__ out
) {
    const int px_x = blockIdx.x * blockDim.x + threadIdx.x;
    const int px_y = blockIdx.y * blockDim.y + threadIdx.y;
    if (px_x >= W || px_y >= H) return;

    const double aspect  = static_cast<double>(W) / static_cast<double>(H);
    const double span_im = scale;
    const double span_re = scale * aspect;
    const double re = (center_re - span_re * 0.5) + (static_cast<double>(px_x) + 0.5) / W * span_re;
    const double im = (center_im + span_im * 0.5) - (static_cast<double>(px_y) + 0.5) / H * span_im;

    // Initialise z and c based on julia flag.
    double zre, zim, cre, cim;
    if (julia) {
        zre = re;   zim = im;
        cre = julia_re_p; cim = julia_im_p;
    } else {
        zre = 0.0;  zim = 0.0;
        cre = re;   cim = im;
    }

    constexpr bool track_min = (MetricId == 1 || MetricId == 3);
    constexpr bool track_max = (MetricId == 2 || MetricId == 3);
    double mn = INFINITY;
    double mx = 0.0;
    double zre2 = zre * zre;
    double zim2 = zim * zim;

    // Apply step THEN check — matches escape_time.hpp CPU convention so that
    // r.iter is identical on both paths (both return i when z_{i+1} escapes).
    int i = 0;
    for (; i < max_iter; i++) {
        double next_re = 0.0;
        double next_im = 0.0;
        step_fp64_cached<VariantId>(zre, zim, zre2, zim2, cre, cim, next_re, next_im);
        const bool finite_z = isfinite(next_re) && isfinite(next_im);
        double next_re2 = 0.0;
        double next_im2 = 0.0;
        const double abs2 = finite_z
            ? ((next_re2 = next_re * next_re) + (next_im2 = next_im * next_im))
            : INFINITY;
        if constexpr (track_min) {
            if (abs2 < mn) mn = abs2;
        }
        if constexpr (track_max) {
            if (abs2 > mx) mx = abs2;
        }
        if (!finite_z || abs2 > bail2) break;
        zre = next_re;
        zim = next_im;
        zre2 = next_re2;
        zim2 = next_im2;
    }

    uint8_t* px = out + (static_cast<size_t>(px_y) * W + px_x) * 3;

    if constexpr (MetricId == 1) {
        const double bailout = sqrt(bail2);
        const double raw = sqrt(mn);
        const double v01 = fmin(1.0, raw / bailout);
        colorize_metric_field_bgr(raw, v01, colormap_id, px);
    } else if constexpr (MetricId == 2) {
        const double bailout = sqrt(bail2);
        const double raw = sqrt(mx);
        const double v01 = fmin(1.0, raw / bailout);
        colorize_metric_field_bgr(raw, v01, colormap_id, px);
    } else if constexpr (MetricId == 3) {
        const double bailout = sqrt(bail2);
        const double raw = 0.5 * (sqrt(mn) + sqrt(mx));
        const double v01 = fmin(1.0, raw / bailout);
        colorize_metric_field_bgr(raw, v01, colormap_id, px);
    } else {
        colorize_escape_bgr(i, max_iter, colormap_id, px);
    }
}

// ---- fixed-point integer kernel ----

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
__device__ inline double fixed_mag2_to_abs_double(uint64_t mag2_raw) {
    return sqrt(static_cast<double>(mag2_raw) / Fixed64<FRAC>::SCALE);
}

template <int FRAC>
__device__ inline double fixed_raw_to_double_u(uint64_t raw) {
    return static_cast<double>(raw) / Fixed64<FRAC>::SCALE;
}

template <int FRAC, int VariantId>
__device__ inline void step_fixed_cached(
    Fixed64<FRAC> zre,
    Fixed64<FRAC> zim,
    Fixed64<FRAC> zre2,
    Fixed64<FRAC> zim2,
    const Fixed64<FRAC>& cre,
    const Fixed64<FRAC>& cim,
    Fixed64<FRAC>& new_re,
    Fixed64<FRAC>& new_im
) {
    using S = Fixed64<FRAC>;
    const S sq_re = zre2 - zim2;
    if constexpr (VariantId == 1) {
        const S sq_im = (zre * zim) + (zre * zim);
        new_re = sq_re + cre;
        new_im = cim - sq_im;
    } else if constexpr (VariantId == 2) {
        const S ax = fixed_abs<FRAC>(zre);
        const S ay = fixed_abs<FRAC>(zim);
        const S sq_im = (ax * ay) + (ax * ay);
        new_re = sq_re + cre;
        new_im = sq_im + cim;
    } else if constexpr (VariantId == 3) {
        const S ay = fixed_abs<FRAC>(zim);
        const S sq_im = (zre * ay) + (zre * ay);
        new_re = sq_re + cre;
        new_im = sq_im + cim;
    } else if constexpr (VariantId == 4) {
        const S ax = fixed_abs<FRAC>(zre);
        const S neg_y = -zim;
        const S sq_im = (ax * neg_y) + (ax * neg_y);
        new_re = sq_re + cre;
        new_im = sq_im + cim;
    } else if constexpr (VariantId == 5) {
        const S sq_im = (zre * zim) + (zre * zim);
        new_re = fixed_abs<FRAC>(sq_re) + cre;
        new_im = sq_im + cim;
    } else if constexpr (VariantId == 6) {
        const S sq_im = (zre * zim) + (zre * zim);
        new_re = fixed_abs<FRAC>(sq_re) + cre;
        new_im = cim - sq_im;
    } else if constexpr (VariantId == 7) {
        const S sq_im = (zre * zim) + (zre * zim);
        new_re = fixed_abs<FRAC>(sq_re) + cre;
        new_im = fixed_abs<FRAC>(sq_im) + cim;
    } else if constexpr (VariantId == 8) {
        const S ay = fixed_abs<FRAC>(zim);
        const S sq_im = (zre * ay) + (zre * ay);
        new_re = fixed_abs<FRAC>(sq_re) + cre;
        new_im = sq_im + cim;
    } else if constexpr (VariantId == 9) {
        const S ax = fixed_abs<FRAC>(zre);
        const S sq_im = (ax * zim) + (ax * zim);
        new_re = fixed_abs<FRAC>(sq_re) + cre;
        new_im = cim - sq_im;
    } else {
        const S sq_im = (zre * zim) + (zre * zim);
        new_re = sq_re + cre;
        new_im = sq_im + cim;
    }
}

template <int FRAC, int VariantId, int MetricId, bool Julia>
__global__ void fractal_fx64_int_kernel(
    Fx64ViewportRaw vp,
    int W, int H, int max_iter, int colormap_id,
    uint8_t* __restrict__ out
) {
    const int px_x = blockIdx.x * blockDim.x + threadIdx.x;
    const int px_y = blockIdx.y * blockDim.y + threadIdx.y;
    if (px_x >= W || px_y >= H) return;

    const int64_t pixel_re_raw = vp.first_re_raw + static_cast<int64_t>(px_x) * vp.step_re_raw;
    const int64_t pixel_im_raw = vp.first_im_raw - static_cast<int64_t>(px_y) * vp.step_im_raw;
    (void)pixel_re_raw;
    (void)pixel_im_raw;

    using S = Fixed64<FRAC>;
    S zre, zim, cre, cim;
    if constexpr (Julia) {
        zre = {pixel_re_raw};
        zim = {pixel_im_raw};
        cre = {vp.julia_re_raw};
        cim = {vp.julia_im_raw};
    } else {
        zre = {0LL};
        zim = {0LL};
        cre = {pixel_re_raw};
        cim = {pixel_im_raw};
    }

    constexpr bool track_min = (MetricId == 1 || MetricId == 3);
    constexpr bool track_max = (MetricId == 2 || MetricId == 3);
    constexpr bool component_gate = (FRAC == 59);
    constexpr bool l1_gate = (FRAC == 60);
    constexpr bool can_gate_without_mag2 = !(track_min || track_max);
    uint64_t mn = UINT64_MAX;
    uint64_t mx = 0;
    bool escaped_initial = false;
    uint64_t zre2_raw = 0;
    uint64_t zim2_raw = 0;
    if constexpr (Julia) {
        uint64_t mag2_raw = 0;
        uint64_t l1_raw = 0;
        bool have_mag2 = false;

        if constexpr (component_gate || l1_gate) {
            const uint64_t ax = abs_i64_to_u64(zre.raw);
            const uint64_t ay = abs_i64_to_u64(zim.raw);
            escaped_initial = ax > vp.bailout_raw || ay > vp.bailout_raw;
            if constexpr (l1_gate) {
                if (!escaped_initial) {
                    l1_raw = add_u64_sat_cuda(ax, ay);
                    escaped_initial = l1_raw > vp.two_sqrt2_floor_raw;
                }
            }
        }

        if constexpr (FRAC == 57 || track_min || track_max) {
            zre2_raw = fixed_square_q_sat_raw_cuda<FRAC>(zre.raw);
            zim2_raw = fixed_square_q_sat_raw_cuda<FRAC>(zim.raw);
            mag2_raw = add_u64_sat_cuda(zre2_raw, zim2_raw);
            have_mag2 = true;
            if constexpr (track_min) mn = mag2_raw < mn ? mag2_raw : mn;
            if constexpr (track_max) mx = mag2_raw > mx ? mag2_raw : mx;
        }

        if (!escaped_initial) {
            if (!have_mag2) {
                zre2_raw = fixed_square_q_sat_raw_cuda<FRAC>(zre.raw);
                zim2_raw = fixed_square_q_sat_raw_cuda<FRAC>(zim.raw);
                mag2_raw = add_u64_sat_cuda(zre2_raw, zim2_raw);
            }
            if constexpr (l1_gate) {
                escaped_initial = l1_raw > vp.two_raw && mag2_raw > vp.bailout2_raw;
            } else {
                escaped_initial = mag2_raw > vp.bailout2_raw;
            }
        } else if (!have_mag2 && (track_min || track_max)) {
            zre2_raw = fixed_square_q_sat_raw_cuda<FRAC>(zre.raw);
            zim2_raw = fixed_square_q_sat_raw_cuda<FRAC>(zim.raw);
            mag2_raw = add_u64_sat_cuda(zre2_raw, zim2_raw);
            if constexpr (track_min) mn = mag2_raw < mn ? mag2_raw : mn;
            if constexpr (track_max) mx = mag2_raw > mx ? mag2_raw : mx;
        }
    } else {
        zre2_raw = 0;
        zim2_raw = 0;
    }

    S zre2{u64_to_i64_sat_cuda(zre2_raw)};
    S zim2{u64_to_i64_sat_cuda(zim2_raw)};

    // Apply step THEN check — matches escape_time.hpp CPU convention.
    int i = 0;
    for (; !escaped_initial && i < max_iter; i++) {
        S next_re{};
        S next_im{};
        step_fixed_cached<FRAC, VariantId>(zre, zim, zre2, zim2, cre, cim, next_re, next_im);

        bool escaped = false;
        uint64_t mag2_raw = 0;
        uint64_t next_re2_raw = 0;
        uint64_t next_im2_raw = 0;
        uint64_t l1_raw = 0;
        if constexpr (can_gate_without_mag2 && (component_gate || l1_gate)) {
            const uint64_t ax = abs_i64_to_u64(next_re.raw);
            const uint64_t ay = abs_i64_to_u64(next_im.raw);
            escaped = ax > vp.bailout_raw || ay > vp.bailout_raw;
            if constexpr (l1_gate) {
                if (!escaped) {
                    l1_raw = add_u64_sat_cuda(ax, ay);
                    escaped = l1_raw > vp.two_sqrt2_floor_raw;
                }
            }
        }
        if constexpr (track_min || track_max) {
            next_re2_raw = fixed_square_q_sat_raw_cuda<FRAC>(next_re.raw);
            next_im2_raw = fixed_square_q_sat_raw_cuda<FRAC>(next_im.raw);
            mag2_raw = add_u64_sat_cuda(next_re2_raw, next_im2_raw);
            if constexpr (track_min) mn = mag2_raw < mn ? mag2_raw : mn;
            if constexpr (track_max) mx = mag2_raw > mx ? mag2_raw : mx;
            escaped = mag2_raw > vp.bailout2_raw;
        } else if (!escaped) {
            next_re2_raw = fixed_square_q_sat_raw_cuda<FRAC>(next_re.raw);
            next_im2_raw = fixed_square_q_sat_raw_cuda<FRAC>(next_im.raw);
            mag2_raw = add_u64_sat_cuda(next_re2_raw, next_im2_raw);
            if constexpr (l1_gate) {
                escaped = l1_raw > vp.two_raw && mag2_raw > vp.bailout2_raw;
            } else {
                escaped = mag2_raw > vp.bailout2_raw;
            }
        }
        if (escaped) break;

        zre = next_re;
        zim = next_im;
        zre2 = {u64_to_i64_sat_cuda(next_re2_raw)};
        zim2 = {u64_to_i64_sat_cuda(next_im2_raw)};
    }

    uint8_t* px = out + (static_cast<size_t>(px_y) * W + px_x) * 3;

    if constexpr (MetricId == 1) {
        const double bailout = fixed_raw_to_double_u<FRAC>(vp.bailout_raw);
        const double raw = fixed_mag2_to_abs_double<FRAC>(mn);
        const double v01 = bailout > 0.0 ? fmin(1.0, raw / bailout) : 0.0;
        colorize_metric_field_bgr(raw, v01, colormap_id, px);
    } else if constexpr (MetricId == 2) {
        const double bailout = fixed_raw_to_double_u<FRAC>(vp.bailout_raw);
        const double raw = fixed_mag2_to_abs_double<FRAC>(mx);
        const double v01 = bailout > 0.0 ? fmin(1.0, raw / bailout) : 0.0;
        colorize_metric_field_bgr(raw, v01, colormap_id, px);
    } else if constexpr (MetricId == 3) {
        const double bailout = fixed_raw_to_double_u<FRAC>(vp.bailout_raw);
        const double raw = 0.5 * (fixed_mag2_to_abs_double<FRAC>(mn) + fixed_mag2_to_abs_double<FRAC>(mx));
        const double v01 = bailout > 0.0 ? fmin(1.0, raw / bailout) : 0.0;
        colorize_metric_field_bgr(raw, v01, colormap_id, px);
    } else {
        colorize_escape_bgr(i, max_iter, colormap_id, px);
    }
}

// ---- Device output buffer (cached across calls of same size) ----

struct DevBuf {
    int W = 0, H = 0;
    uint8_t* d = nullptr;

    void ensure(int w, int h) {
        if (w == W && h == H) return;
        if (d) { cudaFree(d); d = nullptr; }
        CUDA_CHECK(cudaMalloc(&d, static_cast<size_t>(w) * h * 3));
        W = w; H = h;
    }
    void release() { if (d) { cudaFree(d); d = nullptr; W = H = 0; } }
};

static std::mutex  g_cuda_mutex;
static DevBuf      g_devbuf;

// ---- Public API ----

bool cuda_available() noexcept {
    int count = 0;
    return cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

CudaDeviceInfo cuda_device_info() noexcept {
    CudaDeviceInfo info;
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count <= 0) return info;
    info.available = true;
    info.device_count = count;

    int device = 0;
    (void)cudaGetDevice(&device);
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, device) == cudaSuccess) {
        info.major = prop.major;
        info.minor = prop.minor;
        info.total_global_mem = prop.totalGlobalMem;
        info.name = prop.name;
    }

    size_t free_mem = 0, total_mem = 0;
    if (cudaMemGetInfo(&free_mem, &total_mem) == cudaSuccess) {
        info.free_global_mem = free_mem;
        if (info.total_global_mem == 0) info.total_global_mem = total_mem;
    }
    return info;
}

template <int MetricId>
static void launch_fp32_metric(const CudaMapParams& p, dim3 grid, dim3 block, float bail2, uint8_t* out) {
#define FSD_LAUNCH_FP32(VID) \
    fractal_fp32<VID, MetricId><<<grid, block>>>( \
        static_cast<float>(p.center_re), static_cast<float>(p.center_im), static_cast<float>(p.scale), \
        p.width, p.height, p.iterations, bail2, p.colormap_id, \
        p.julia, static_cast<float>(p.julia_re), static_cast<float>(p.julia_im), out)

    switch (p.variant_id) {
        case 1:  FSD_LAUNCH_FP32(1); break;
        case 2:  FSD_LAUNCH_FP32(2); break;
        case 3:  FSD_LAUNCH_FP32(3); break;
        case 4:  FSD_LAUNCH_FP32(4); break;
        case 5:  FSD_LAUNCH_FP32(5); break;
        case 6:  FSD_LAUNCH_FP32(6); break;
        case 7:  FSD_LAUNCH_FP32(7); break;
        case 8:  FSD_LAUNCH_FP32(8); break;
        case 9:  FSD_LAUNCH_FP32(9); break;
        default: FSD_LAUNCH_FP32(0); break;
    }
#undef FSD_LAUNCH_FP32
}

static void launch_fp32(const CudaMapParams& p, dim3 grid, dim3 block, float bail2, uint8_t* out) {
    switch (p.metric_id) {
        case 1:  launch_fp32_metric<1>(p, grid, block, bail2, out); break;
        case 2:  launch_fp32_metric<2>(p, grid, block, bail2, out); break;
        case 3:  launch_fp32_metric<3>(p, grid, block, bail2, out); break;
        default: launch_fp32_metric<0>(p, grid, block, bail2, out); break;
    }
}

template <int MetricId>
static void launch_fp64_metric(const CudaMapParams& p, dim3 grid, dim3 block, double bail2, uint8_t* out) {
#define FSD_LAUNCH_FP64(VID) \
    fractal_fp64<VID, MetricId><<<grid, block>>>( \
        p.center_re, p.center_im, p.scale, \
        p.width, p.height, p.iterations, bail2, p.colormap_id, \
        p.julia, p.julia_re, p.julia_im, out)

    switch (p.variant_id) {
        case 1:  FSD_LAUNCH_FP64(1); break;
        case 2:  FSD_LAUNCH_FP64(2); break;
        case 3:  FSD_LAUNCH_FP64(3); break;
        case 4:  FSD_LAUNCH_FP64(4); break;
        case 5:  FSD_LAUNCH_FP64(5); break;
        case 6:  FSD_LAUNCH_FP64(6); break;
        case 7:  FSD_LAUNCH_FP64(7); break;
        case 8:  FSD_LAUNCH_FP64(8); break;
        case 9:  FSD_LAUNCH_FP64(9); break;
        default: FSD_LAUNCH_FP64(0); break;
    }
#undef FSD_LAUNCH_FP64
}

static void launch_fp64(const CudaMapParams& p, dim3 grid, dim3 block, double bail2, uint8_t* out) {
    switch (p.metric_id) {
        case 1:  launch_fp64_metric<1>(p, grid, block, bail2, out); break;
        case 2:  launch_fp64_metric<2>(p, grid, block, bail2, out); break;
        case 3:  launch_fp64_metric<3>(p, grid, block, bail2, out); break;
        default: launch_fp64_metric<0>(p, grid, block, bail2, out); break;
    }
}

template <int FRAC, int VariantId, int MetricId, bool Julia>
static void launch_fixed_one(const CudaMapParams& p, dim3 grid, dim3 block, uint8_t* out) {
    fractal_fx64_int_kernel<FRAC, VariantId, MetricId, Julia><<<grid, block>>>(
        p.fx64_viewport, p.width, p.height, p.iterations, p.colormap_id, out);
}

template <int FRAC, int VariantId, int MetricId>
static void launch_fixed_julia(const CudaMapParams& p, dim3 grid, dim3 block, uint8_t* out) {
    if (p.julia) launch_fixed_one<FRAC, VariantId, MetricId, true>(p, grid, block, out);
    else         launch_fixed_one<FRAC, VariantId, MetricId, false>(p, grid, block, out);
}

template <int FRAC, int MetricId>
static void launch_fixed_metric(const CudaMapParams& p, dim3 grid, dim3 block, uint8_t* out) {
#define FSD_LAUNCH_FX64(VID) \
    launch_fixed_julia<FRAC, VID, MetricId>(p, grid, block, out)

    switch (p.variant_id) {
        case 1:  FSD_LAUNCH_FX64(1); break;
        case 2:  FSD_LAUNCH_FX64(2); break;
        case 3:  FSD_LAUNCH_FX64(3); break;
        case 4:  FSD_LAUNCH_FX64(4); break;
        case 5:  FSD_LAUNCH_FX64(5); break;
        case 6:  FSD_LAUNCH_FX64(6); break;
        case 7:  FSD_LAUNCH_FX64(7); break;
        case 8:  FSD_LAUNCH_FX64(8); break;
        case 9:  FSD_LAUNCH_FX64(9); break;
        default: FSD_LAUNCH_FX64(0); break;
    }
#undef FSD_LAUNCH_FX64
}

template <int FRAC>
static void launch_fixed(const CudaMapParams& p, dim3 grid, dim3 block, uint8_t* out) {
    switch (p.metric_id) {
        case 1:  launch_fixed_metric<FRAC, 1>(p, grid, block, out); break;
        case 2:  launch_fixed_metric<FRAC, 2>(p, grid, block, out); break;
        case 3:  launch_fixed_metric<FRAC, 3>(p, grid, block, out); break;
        default: launch_fixed_metric<FRAC, 0>(p, grid, block, out); break;
    }
}

CudaMapStats cuda_render_map(const CudaMapParams& p, cv::Mat& out) {
    if (!cuda_available()) throw std::runtime_error("CUDA not available");

    const bool use_fp32 = (p.scalar_type == "fp32" ||
                           p.scalar_type == "float32" ||
                           p.scalar_type == "float");
    const bool use_q360 = (p.scalar_type == "q3.60" || p.scalar_type == "q360" ||
                           p.scalar_type == "fx60" || p.scalar_type == "fixed60");
    const bool use_q459 = (p.scalar_type == "q4.59" || p.scalar_type == "q459" ||
                           p.scalar_type == "fx59" || p.scalar_type == "fixed59");
    const bool use_fx = use_q360 || use_q459 || p.scalar_type == "fx64" ||
                        p.scalar_type == "q6.57" || p.scalar_type == "q657" ||
                        p.scalar_type == "fixed57";

    std::lock_guard<std::mutex> lock(g_cuda_mutex);

    // Ensure device buffer is large enough.
    g_devbuf.ensure(p.width, p.height);

    const dim3 block(32, 8);
    const dim3 grid(
        (p.width + static_cast<int>(block.x) - 1) / static_cast<int>(block.x),
        (p.height + static_cast<int>(block.y) - 1) / static_cast<int>(block.y));
    const double bail2 = p.bailout_sq;

    // Time the kernel with CUDA events.
    cudaEvent_t ev_start, ev_stop;
    CUDA_CHECK(cudaEventCreate(&ev_start));
    CUDA_CHECK(cudaEventCreate(&ev_stop));
    CUDA_CHECK(cudaEventRecord(ev_start));

    if (use_fp32) {
        launch_fp32(p, grid, block, static_cast<float>(bail2), g_devbuf.d);
    } else if (use_q360) {
        launch_fixed<60>(p, grid, block, g_devbuf.d);
    } else if (use_q459) {
        launch_fixed<59>(p, grid, block, g_devbuf.d);
    } else if (use_fx) {
        launch_fixed<57>(p, grid, block, g_devbuf.d);
    } else {
        launch_fp64(p, grid, block, bail2, g_devbuf.d);
    }

    CUDA_CHECK(cudaEventRecord(ev_stop));
    CUDA_CHECK(cudaEventSynchronize(ev_stop));

    float ms = 0.0f;
    CUDA_CHECK(cudaEventElapsedTime(&ms, ev_start, ev_stop));
    CUDA_CHECK(cudaEventDestroy(ev_start));
    CUDA_CHECK(cudaEventDestroy(ev_stop));

    // Copy result to host Mat.
    const size_t nbytes = static_cast<size_t>(p.width) * p.height * 3;
    if (out.empty() || out.rows != p.height || out.cols != p.width || out.type() != CV_8UC3)
        out.create(p.height, p.width, CV_8UC3);
    CUDA_CHECK(cudaMemcpy(out.data, g_devbuf.d, nbytes, cudaMemcpyDeviceToHost));

    CudaMapStats s;
    s.elapsed_ms  = static_cast<double>(ms);
    s.scalar_used = use_fp32 ? "fp32" : (use_q360 ? "q3.60" : (use_q459 ? "q4.59" : (use_fx ? "fx64" : "fp64")));
    s.engine_used = "cuda";
    return s;
}

} // namespace fsd_cuda
