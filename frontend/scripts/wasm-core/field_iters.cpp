// Minimal freestanding fractal iteration core for wasm SIMD benchmarking.
// Same math as local-render-core.iterateOrbit (fp64 mandelbrot escape).
// Tiled: renders tileHeight rows starting at global row y0, so multiple
// workers can split one frame.
#include <stdint.h>

typedef double v2d __attribute__((vector_size(16)));

static inline void step(v2d& zre, v2d& zim, const v2d& cre, const v2d& cim) {
    const v2d two = {2.0, 2.0};
    v2d next_re = zre * zre - zim * zim + cre;
    v2d next_im = two * zre * zim + cim;
    zre = next_re;
    zim = next_im;
}

// SIMD f64x2: two pixels per vector (lanes hold re of two pixels, im of two).
extern "C" void field_iters_simd(
    double centerRe, double centerIm, double scale,
    uint32_t iterations, double bailout,
    uint32_t width, uint32_t globalHeight, uint32_t y0, uint32_t tileHeight,
    uint32_t* out
) {
    const double aspect = (double)width / globalHeight;
    const double bailout_sq = bailout * bailout;
    for (uint32_t y = 0; y < tileHeight; ++y) {
        const double im0 = centerIm + (0.5 - ((double)(y0 + y) + 0.5) / globalHeight) * scale;
        for (uint32_t x = 0; x < width; x += 2) {
            const double re0 = centerRe + (((double)x + 0.5) / width - 0.5) * scale * aspect;
            const double re1 = centerRe + (((double)x + 1.5) / width - 0.5) * scale * aspect;
            const v2d cre = {re0, re1};
            const v2d cim = {im0, im0};
            v2d zre = {0.0, 0.0};
            v2d zim = {0.0, 0.0};
            uint32_t it0 = iterations, it1 = iterations;
            for (uint32_t i = 0; i < iterations; ++i) {
                step(zre, zim, cre, cim);
                v2d norm = zre * zre + zim * zim;
                if (it0 == iterations && norm[0] > bailout_sq) it0 = i;
                if (it1 == iterations && norm[1] > bailout_sq) it1 = i;
                if (it0 != iterations && it1 != iterations) break;
            }
            out[y * width + x] = it0;
            if (x + 1 < width) out[y * width + x + 1] = it1;
        }
    }
}

// Scalar reference: same math, one pixel at a time (SIMD speedup baseline).
extern "C" void field_iters_scalar(
    double centerRe, double centerIm, double scale,
    uint32_t iterations, double bailout,
    uint32_t width, uint32_t globalHeight, uint32_t y0, uint32_t tileHeight,
    uint32_t* out
) {
    const double aspect = (double)width / globalHeight;
    const double bailout_sq = bailout * bailout;
    for (uint32_t y = 0; y < tileHeight; ++y) {
        const double im = centerIm + (0.5 - ((double)(y0 + y) + 0.5) / globalHeight) * scale;
        for (uint32_t x = 0; x < width; ++x) {
            const double re = centerRe + (((double)x + 0.5) / width - 0.5) * scale * aspect;
            double zre = 0.0, zim = 0.0;
            uint32_t it = iterations;
            for (uint32_t i = 0; i < iterations; ++i) {
                double nre = zre * zre - zim * zim + re;
                double nim = 2.0 * zre * zim + im;
                zre = nre;
                zim = nim;
                double norm = zre * zre + zim * zim;
                if (norm > bailout_sq) { it = i; break; }
            }
            out[y * width + x] = it;
        }
    }
}
