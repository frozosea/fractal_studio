// Full browser-side fractal field core, compiled to wasm SIMD.
// Mirrors local-render-core.iterateOrbit / computeLocalRawField exactly
// (same IEEE operation order, fp64), for the 10 quadratic 2D variants and
// escape/min_abs/max_abs/envelope metrics. Transcendental variants, orbit
// programs, transitions and min_pairwise_dist stay on the JS core.
//
// Outputs three tiled arrays (width x tileHeight):
//   out_iters  u32  escape iteration count
//   out_norms  f32  escape norm^2 (0 when not escaped)
//   out_fields f64  metric field (min_abs/max_abs/envelope; 0 for escape)
//
// Build:
//   clang --target=wasm32 -O3 -msimd128 -nostdlib -Wl,--no-entry \
//     -Wl,--export=field_core_render -Wl,--export-memory \
//     -Wl,--initial-memory=33554432 -o field_core.wasm field_core.cpp
#include <stdint.h>

typedef double v2d __attribute__((vector_size(16)));

static inline v2d vabs2(v2d x) {
    return (v2d){__builtin_fabs(x[0]), __builtin_fabs(x[1])};
}

// Step for two pixels in parallel (SIMD): lane 0 = pixel A, lane 1 = pixel B.
// Operation order matches the TypeScript step() exactly.
static inline void step2(int variant, v2d& x, v2d& y, const v2d& cx, const v2d& cy) {
    const v2d x2 = x * x;
    const v2d y2 = y * y;
    const v2d xy2 = (v2d){2.0, 2.0} * x * y;
    v2d nx, ny;
    switch (variant) {
        case 0: nx = x2 - y2 + cx; ny = xy2 + cy; break;                                   // mandelbrot
        case 1: nx = x2 - y2 + cx; ny = (v2d){0.0, 0.0} - xy2 + cy; break;                 // tricorn
        case 2: nx = x2 - y2 + cx; ny = (v2d){2.0, 2.0} * vabs2(x) * vabs2(y) + cy; break; // burning_ship
        case 3: nx = x2 - y2 + cx; ny = (v2d){2.0, 2.0} * x * vabs2(y) + cy; break;        // celtic
        case 4: nx = x2 - y2 + cx; ny = (v2d){0.0, 0.0} - (v2d){2.0, 2.0} * vabs2(x) * y + cy; break; // heart
        case 5: nx = vabs2(x2 - y2) + cx; ny = xy2 + cy; break;                            // buffalo
        case 6: nx = vabs2(x2 - y2) + cx; ny = (v2d){0.0, 0.0} - xy2 + cy; break;          // perp_buffalo
        case 7: nx = vabs2(x2 - y2) + cx; ny = vabs2(xy2) + cy; break;                     // celtic_ship
        case 8: nx = vabs2(x2 - y2) + cx; ny = (v2d){2.0, 2.0} * x * vabs2(y) + cy; break; // mandelceltic
        case 9: nx = vabs2(x2 - y2) + cx; ny = (v2d){0.0, 0.0} - (v2d){2.0, 2.0} * vabs2(x) * y + cy; break; // perp_ship
        default: nx = cx; ny = cy; break;
    }
    x = nx; y = ny;
}

// scalar twin of step2 for the non-SIMD path (same math per pixel)
static inline void step1(int variant, double& x, double& y, double cx, double cy) {
    const double x2 = x * x;
    const double y2 = y * y;
    const double xy2 = 2.0 * x * y;
    double nx, ny;
    switch (variant) {
        case 0: nx = x2 - y2 + cx; ny = xy2 + cy; break;
        case 1: nx = x2 - y2 + cx; ny = -xy2 + cy; break;
        case 2: nx = x2 - y2 + cx; ny = 2.0 * __builtin_fabs(x) * __builtin_fabs(y) + cy; break;
        case 3: nx = x2 - y2 + cx; ny = 2.0 * x * __builtin_fabs(y) + cy; break;
        case 4: nx = x2 - y2 + cx; ny = -2.0 * __builtin_fabs(x) * y + cy; break;
        case 5: nx = __builtin_fabs(x2 - y2) + cx; ny = xy2 + cy; break;
        case 6: nx = __builtin_fabs(x2 - y2) + cx; ny = -xy2 + cy; break;
        case 7: nx = __builtin_fabs(x2 - y2) + cx; ny = __builtin_fabs(xy2) + cy; break;
        case 8: nx = __builtin_fabs(x2 - y2) + cx; ny = 2.0 * x * __builtin_fabs(y) + cy; break;
        case 9: nx = __builtin_fabs(x2 - y2) + cx; ny = -2.0 * __builtin_fabs(x) * y + cy; break;
        default: nx = cx; ny = cy; break;
    }
    x = nx; y = ny;
}

extern "C" void field_core_render(
    double centerRe, double centerIm, double scale, double aspect,
    double cosAngle, double sinAngle,
    uint32_t iterations, double bailout,
    uint32_t variant, uint32_t metric,
    uint32_t julia, double juliaRe, double juliaIm,
    uint32_t width, uint32_t globalHeight, uint32_t y0, uint32_t tileHeight,
    uint32_t* out_iters, float* out_norms, double* out_fields
) {
    const double bailout2 = bailout * bailout;
    const v2d b2v = {bailout2, bailout2};
    const v2d maxd = {1.7976931348623157e308, 1.7976931348623157e308};
    const uint32_t useSimd = 1;  // both paths available; SIMD used unless width odd
    for (uint32_t y = 0; y < tileHeight; ++y) {
        const double localIm = (0.5 - ((double)(y0 + y) + 0.5) / globalHeight) * scale;
        for (uint32_t x = 0; x < width; x += 2) {
            const bool hasSecond = (x + 1) < width;
            const double localRe0 = (((double)x + 0.5) / width - 0.5) * scale * aspect;
            const double localRe1 = hasSecond ? (((double)(x + 1) + 0.5) / width - 0.5) * scale * aspect : 0.0;
            // Rotation rotates the local offset only; the center stays fixed
            // (matches computeLocalRawField: center + R * local).
            const double rr0 = centerRe + localRe0 * cosAngle - localIm * sinAngle;
            const double ri0 = centerIm + localRe0 * sinAngle + localIm * cosAngle;
            const double rr1 = hasSecond ? centerRe + localRe1 * cosAngle - localIm * sinAngle : 0.0;
            const double ri1 = hasSecond ? centerIm + localRe1 * sinAngle + localIm * cosAngle : 0.0;
            const uint32_t idx = y * width + x;
            if (hasSecond) {
                v2d zx = {0.0, 0.0};
                v2d zy = {0.0, 0.0};
                v2d cx, cy;
                if (julia) { zx = {rr0, rr1}; zy = {ri0, ri1}; cx = {juliaRe, juliaRe}; cy = {juliaIm, juliaIm}; }
                else { cx = {rr0, rr1}; cy = {ri0, ri1}; }
                v2d minimum = maxd;
                v2d maximum = {0.0, 0.0};
                uint32_t it0 = iterations, it1 = iterations;
                v2d norm = {0.0, 0.0};
                double esc_norm0 = 0.0, esc_norm1 = 0.0;
                for (uint32_t i = 0; i < iterations; ++i) {
                    step2((int)variant, zx, zy, cx, cy);
                    norm = zx * zx + zy * zy;
                    // A lane stops accumulating once it escapes (matches the
                    // per-pixel TS core which returns at escape).
                    if (it0 == iterations) {
                        if (norm[0] < minimum[0]) minimum[0] = norm[0];
                        if (norm[0] > maximum[0]) maximum[0] = norm[0];
                        if (norm[0] > bailout2) { it0 = i; esc_norm0 = norm[0]; }
                    }
                    if (it1 == iterations) {
                        if (norm[1] < minimum[1]) minimum[1] = norm[1];
                        if (norm[1] > maximum[1]) maximum[1] = norm[1];
                        if (norm[1] > bailout2) { it1 = i; esc_norm1 = norm[1]; }
                    }
                    if (it0 != iterations && it1 != iterations) break;
                }
                out_iters[idx] = it0;
                out_iters[idx + 1] = it1;
                out_norms[idx] = it0 < iterations ? (float)esc_norm0 : 0.0f;
                out_norms[idx + 1] = it1 < iterations ? (float)esc_norm1 : 0.0f;
                const double f0 = (metric == 1) ? (minimum[0] <= 1.7976931348623157e308 ? __builtin_sqrt(minimum[0]) : 0.0)
                    : (metric == 2) ? (maximum[0] > 0.0 ? __builtin_sqrt(maximum[0]) : 0.0)
                    : (metric == 3) ? 0.5 * ((minimum[0] <= 1.7976931348623157e308 ? __builtin_sqrt(minimum[0]) : 0.0) + (maximum[0] > 0.0 ? __builtin_sqrt(maximum[0]) : 0.0)) : 0.0;
                const double f1 = (metric == 1) ? (minimum[1] <= 1.7976931348623157e308 ? __builtin_sqrt(minimum[1]) : 0.0)
                    : (metric == 2) ? (maximum[1] > 0.0 ? __builtin_sqrt(maximum[1]) : 0.0)
                    : (metric == 3) ? 0.5 * ((minimum[1] <= 1.7976931348623157e308 ? __builtin_sqrt(minimum[1]) : 0.0) + (maximum[1] > 0.0 ? __builtin_sqrt(maximum[1]) : 0.0)) : 0.0;
                out_fields[idx] = f0;
                out_fields[idx + 1] = f1;
            } else {
                // odd width tail: scalar path
                double zx = julia ? rr0 : 0.0;
                double zy = julia ? ri0 : 0.0;
                double cx = julia ? juliaRe : rr0;
                double cy = julia ? juliaIm : ri0;
                double minimum = 1.7976931348623157e308;
                double maximum = 0.0;
                uint32_t it = iterations;
                double norm = 0.0;
                for (uint32_t i = 0; i < iterations; ++i) {
                    step1((int)variant, zx, zy, cx, cy);
                    norm = zx * zx + zy * zy;
                    if (norm < minimum) minimum = norm;
                    if (norm > maximum) maximum = norm;
                    if (norm > bailout2) { it = i; break; }
                }
                out_iters[idx] = it;
                out_norms[idx] = it < iterations ? (float)norm : 0.0f;
                out_fields[idx] = (metric == 1) ? (minimum <= 1.7976931348623157e308 ? __builtin_sqrt(minimum) : 0.0)
                    : (metric == 2) ? (maximum > 0.0 ? __builtin_sqrt(maximum) : 0.0)
                    : (metric == 3) ? 0.5 * ((minimum <= 1.7976931348623157e308 ? __builtin_sqrt(minimum) : 0.0) + (maximum > 0.0 ? __builtin_sqrt(maximum) : 0.0)) : 0.0;
            }
        }
    }
}
