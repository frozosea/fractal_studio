// compute/colormap.hpp
//
// Per-pixel colormap + metric-aware colorize(). These are lifted from the
// legacy managed map renderer (fractal_studio/backend/src/adapters/
// legacy_map_adapter.cpp:80-149) and kept pixel-exact so we can regression-
// test new output against the legacy PNGs.

#pragma once

#include "escape_time.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace fsd::compute {

enum class Colormap {
    ClassicCos = 0,
    Mod17      = 1,
    HsvWheel   = 2,
    Tri765     = 3,
    Grayscale  = 4,
    HsRainbow  = 5,   // 6-band rainbow with log-scale index (matches legacy boat_3D6_2D_enlarge.c)
    Inferno    = 6,
    Viridis    = 7,
    Twilight   = 8,
    EmberBlue  = 9,
};

inline bool colormap_from_name(const char* name, Colormap& out) {
    struct Entry { const char* n; Colormap c; };
    static constexpr Entry table[] = {
        {"classic_cos", Colormap::ClassicCos},
        {"mod17",       Colormap::Mod17},
        {"hsv_wheel",   Colormap::HsvWheel},
        {"tri765",      Colormap::Tri765},
        {"grayscale",   Colormap::Grayscale},
        {"hs_rainbow",  Colormap::HsRainbow},
        {"inferno",     Colormap::Inferno},
        {"viridis",     Colormap::Viridis},
        {"twilight",    Colormap::Twilight},
        {"ember_blue",  Colormap::EmberBlue},
    };
    for (const auto& e : table) {
        const char* a = e.n;
        const char* b = name;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (*a == 0 && *b == 0) { out = e.c; return true; }
    }
    return false;
}

// Compute smooth (continuous) iteration count.
// mu = iter + 1 - log2(log2(|z|²))
// Eliminates integer-step banding when escape radius is large enough.
// Returns raw iter count if norm <= 1 (no escape or tiny escape radius).
inline double smooth_mu(int iter, double norm) {
    if (norm > 1.0) {
        const double mu = static_cast<double>(iter) + 1.0 - std::log2(std::log2(norm));
        return mu > 0.0 ? mu : 0.0;
    }
    return static_cast<double>(iter);
}

inline int clamp255(int v) {
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return v;
}

inline double cos_color(double n, double freq) {
    constexpr double PI = 3.141592653589793;
    return 128.0 - 128.0 * std::cos(freq * n * PI);
}

struct ColorStop {
    double t;
    int r;
    int g;
    int b;
};

template <size_t N>
inline void gradient_stops_bgr(double t, const ColorStop (&stops)[N], uint8_t& b, uint8_t& g, uint8_t& r) {
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    if (t <= stops[0].t) {
        r = static_cast<uint8_t>(clamp255(stops[0].r));
        g = static_cast<uint8_t>(clamp255(stops[0].g));
        b = static_cast<uint8_t>(clamp255(stops[0].b));
        return;
    }
    for (size_t i = 1; i < N; ++i) {
        if (t <= stops[i].t) {
            const double span = std::max(1e-12, stops[i].t - stops[i - 1].t);
            const double u = (t - stops[i - 1].t) / span;
            const auto mix = [u](int a, int z) {
                return clamp255(static_cast<int>(std::lround(static_cast<double>(a) * (1.0 - u) + static_cast<double>(z) * u)));
            };
            r = static_cast<uint8_t>(mix(stops[i - 1].r, stops[i].r));
            g = static_cast<uint8_t>(mix(stops[i - 1].g, stops[i].g));
            b = static_cast<uint8_t>(mix(stops[i - 1].b, stops[i].b));
            return;
        }
    }
    r = static_cast<uint8_t>(clamp255(stops[N - 1].r));
    g = static_cast<uint8_t>(clamp255(stops[N - 1].g));
    b = static_cast<uint8_t>(clamp255(stops[N - 1].b));
}

inline bool colorize_gradient_palette_bgr(double t, Colormap palette, uint8_t& b, uint8_t& g, uint8_t& r) {
    static constexpr ColorStop inferno[] = {
        {0.00,   0,   0,   4},
        {0.14,  31,  12,  72},
        {0.28,  85,  15, 109},
        {0.42, 136,  34, 106},
        {0.56, 186,  54,  85},
        {0.70, 227,  89,  51},
        {0.84, 249, 140,  10},
        {0.94, 252, 195,  55},
        {1.00, 252, 255, 164},
    };
    static constexpr ColorStop viridis[] = {
        {0.00,  68,   1,  84},
        {0.25,  59,  82, 139},
        {0.50,  33, 145, 140},
        {0.75,  94, 201,  98},
        {1.00, 253, 231,  37},
    };
    static constexpr ColorStop twilight[] = {
        {0.00,  32,  24,  70},
        {0.18,  63,  92, 180},
        {0.36,  58, 150, 165},
        {0.54, 240, 210, 120},
        {0.72, 210,  90,  90},
        {0.88,  90,  50, 110},
        {1.00,  32,  24,  70},
    };
    static constexpr ColorStop ember_blue[] = {
        {0.00,   5,   8,  32},
        {0.22,  10,  70, 120},
        {0.48,  55, 190, 185},
        {0.72, 245, 172,  75},
        {1.00, 255, 246, 210},
    };

    switch (palette) {
        case Colormap::Inferno:   gradient_stops_bgr(t, inferno, b, g, r); return true;
        case Colormap::Viridis:   gradient_stops_bgr(t, viridis, b, g, r); return true;
        case Colormap::Twilight:  gradient_stops_bgr(t, twilight, b, g, r); return true;
        case Colormap::EmberBlue: gradient_stops_bgr(t, ember_blue, b, g, r); return true;
        default: return false;
    }
}

inline void hsv_to_rgb(double h, double s, double v, int& r, int& g, int& b) {
    const double c  = v * s;
    const double hh = h / 60.0;
    const double x  = c * (1.0 - std::fabs(std::fmod(hh, 2.0) - 1.0));

    double rr = 0, gg = 0, bb = 0;
    if (hh < 1.0)      { rr = c; gg = x; }
    else if (hh < 2.0) { rr = x; gg = c; }
    else if (hh < 3.0) { gg = c; bb = x; }
    else if (hh < 4.0) { gg = x; bb = c; }
    else if (hh < 5.0) { rr = x; bb = c; }
    else               { rr = c; bb = x; }

    const double m = v - c;
    r = clamp255(static_cast<int>((rr + m) * 255.0));
    g = clamp255(static_cast<int>((gg + m) * 255.0));
    b = clamp255(static_cast<int>((bb + m) * 255.0));
}

// 6-band rainbow from the legacy boat_3D6_2D_enlarge.c color scheme.
// idx in [0, 1785]:  0→black, 255→blue, 510→purple, 765→red,
//                   1020→green, 1275→cyan, 1530→yellow→1785→white
// BGR output.
inline void rainbow_from_index(int idx, uint8_t& b, uint8_t& g, uint8_t& r) {
    if (idx <= 0)    { r = g = b = 0;   return; }
    if (idx >= 1785) { r = g = b = 255; return; }

    // a0 = blue channel seed, a1 = red channel seed, a2 = green channel seed
    int a0 = idx, a1 = 0, a2 = 0;

    if      (255 < a0 && a0 < 510)  { a1 = a0 - 255; a0 = 510 - a0; }
    else if (509 < a0 && a0 < 765)  { a1 = 255; a0 = a0 - 510; }
    else if (764 < a0 && a0 < 1020) { a2 = a0 - 765; a1 = 1020 - a0; a0 = a1; }
    else if (1019 < a0 && a0 < 1275){ a2 = 255; a0 = a0 - 1020; }
    else if (1274 < a0 && a0 < 1530){ a2 = 255; a1 = a0 - 1275; a0 = 1530 - a0; }
    else if (a0 > 1529)              { a2 = 255; a1 = 255; a0 = a0 - 1530; }

    // Legacy output order: png[R]=a1, png[G]=a2, png[B]=a0  →  BGR: b=a0, g=a2, r=a1
    b = static_cast<uint8_t>(clamp255(a0));
    g = static_cast<uint8_t>(clamp255(a2));
    r = static_cast<uint8_t>(clamp255(a1));
}

// Colorize a field value x (min|z|, max|z|, etc.) with the HsRainbow palette.
// Matches legacy: index = int((36/35 − log2(x)) × 35)
// x == 0 or non-finite → white; x > 2 (outside typical range) → black.
inline void colorize_field_hs_bgr(
    double x,
    uint8_t& b, uint8_t& g, uint8_t& r
) {
    if (x <= 0.0 || !std::isfinite(x)) { r = g = b = 255; return; }
    // index = int( (36/35 - log2(x)) * 35 )  clamped to [0, 1785]
    const double raw = (36.0 / 35.0 - std::log2(x)) * 35.0;
    const int idx = std::max(0, std::min(1785, static_cast<int>(raw)));
    rainbow_from_index(idx, b, g, r);
}

// Colorize classic escape-time iteration. Matches legacy colorize() bit-exact.
//
// smooth: when true, uses μ = iter + 1 − log₂(log₂(|z|²)) as the normalized
//   position instead of n = (iter+1)/(max_iter+2). Eliminates integer banding.
//   Meaningful for HsvWheel, Tri765, Grayscale; ClassicCos and Mod17 also work.
//   norm must be |z|² at escape; pass 0.0 if unavailable (smooth will degrade
//   gracefully to integer iter).
// Writes BGR (OpenCV order).
inline void colorize_escape_bgr(
    int iter, int max_iter, Colormap palette, double norm, bool smooth,
    uint8_t& b, uint8_t& g, uint8_t& r
) {
    if (iter >= max_iter) {
        r = g = b = 255;   // interior: white
        return;
    }

    if (smooth) {
        // Compute continuous μ; cycle every 32 smooth-iters (arbitrary period).
        const double mu = smooth_mu(iter, norm);
        const double t  = std::fmod(mu / 32.0, 1.0);   // [0, 1)
        if (colorize_gradient_palette_bgr(t, palette, b, g, r)) return;

        switch (palette) {
            case Colormap::HsvWheel: {
                int rr = 0, gg = 0, bb = 0;
                hsv_to_rgb(t * 360.0, 1.0, 1.0, rr, gg, bb);
                r = static_cast<uint8_t>(rr);
                g = static_cast<uint8_t>(gg);
                b = static_cast<uint8_t>(bb);
                return;
            }
            case Colormap::Tri765: {
                // Map t → [0, 765)
                const double mf  = t * 765.0;
                const int    m   = static_cast<int>(mf);
                const int    d   = static_cast<int>((mf - m) * 255.0); // sub-band interp
                const int    band = (m / 255) % 3;
                // Use fractional d for smooth interpolation within each band.
                int rr, gg, bb;
                if      (band == 0) { rr = 255 - d; gg = d;       bb = 255;     }
                else if (band == 1) { rr = d;       gg = 255;     bb = 255 - d; }
                else                { rr = 255;     gg = 255 - d; bb = d;       }
                r = static_cast<uint8_t>(clamp255(rr));
                g = static_cast<uint8_t>(clamp255(gg));
                b = static_cast<uint8_t>(clamp255(bb));
                return;
            }
            case Colormap::Grayscale: {
                const int v = clamp255(static_cast<int>(t * 255.0));
                r = g = b = static_cast<uint8_t>(v);
                return;
            }
            case Colormap::ClassicCos:
            default: {
                r = static_cast<uint8_t>(clamp255(static_cast<int>(cos_color(static_cast<double>(t),  53.0))));
                g = static_cast<uint8_t>(clamp255(static_cast<int>(cos_color(static_cast<double>(t),  27.0))));
                b = static_cast<uint8_t>(clamp255(static_cast<int>(cos_color(static_cast<double>(t), 139.0))));
                return;
            }
            case Colormap::Mod17: {
                const int idx = static_cast<int>(mu) % 17;
                const int v   = idx * 15;   // 0..255 in 17 steps
                r = g = b = static_cast<uint8_t>(clamp255(v));
                return;
            }
        }
    }

    // Non-smooth path (original palette formulas).
    const double n = (static_cast<double>(iter) + 1.0) / (static_cast<double>(max_iter) + 2.0);
    if (colorize_gradient_palette_bgr(n, palette, b, g, r)) return;

    switch (palette) {
        case Colormap::Mod17: {
            r = static_cast<uint8_t>(clamp255(iter % 256));
            g = static_cast<uint8_t>(clamp255(iter / 256));
            b = static_cast<uint8_t>(clamp255((iter % 17) * 17));
            return;
        }
        case Colormap::HsvWheel: {
            const double h = std::fmod(static_cast<double>(iter), 1440.0) / 4.0;
            int rr = 0, gg = 0, bb = 0;
            hsv_to_rgb(h, 1.0, 1.0, rr, gg, bb);
            r = static_cast<uint8_t>(rr);
            g = static_cast<uint8_t>(gg);
            b = static_cast<uint8_t>(bb);
            return;
        }
        case Colormap::Tri765: {
            const int m    = iter % 765;
            const int band = m / 255;
            const int d    = m % 255;
            int rr = 255, gg = 255, bb = 255;
            if      (band == 0) { rr = 255 - d; gg = d;       bb = 255;     }
            else if (band == 1) { rr = d;       gg = 255;     bb = 255 - d; }
            else                { rr = 255;     gg = 255 - d; bb = d;       }
            r = static_cast<uint8_t>(clamp255(rr));
            g = static_cast<uint8_t>(clamp255(gg));
            b = static_cast<uint8_t>(clamp255(bb));
            return;
        }
        case Colormap::Grayscale: {
            const int v = clamp255(static_cast<int>(n * 255.0));
            r = g = b = static_cast<uint8_t>(v);
            return;
        }
        case Colormap::ClassicCos:
        default: {
            r = static_cast<uint8_t>(clamp255(static_cast<int>(cos_color(n,  53.0))));
            g = static_cast<uint8_t>(clamp255(static_cast<int>(cos_color(n,  27.0))));
            b = static_cast<uint8_t>(clamp255(static_cast<int>(cos_color(n, 139.0))));
            return;
        }
    }
}

// Ln-smooth coloring for non-escape metrics.
// x is the raw field value (min|z|, max|z|, etc.).
//   x == 0 → white (orbit passed through origin)
//   x  > 0 → idx = int(k * (2 - log2(x))), then apply palette cyclically
// k is chosen per palette so one palette cycle spans ~8 halvings of x (~factor 256).
inline void colorize_field_smooth_bgr(
    double x, Colormap palette,
    uint8_t& b, uint8_t& g, uint8_t& r
) {
    if (x <= 0.0) { r = g = b = 255; return; }   // white for zero / invalid

    const double base_val = 2.0 - std::log2(x);  // = 2 - log2(x)
    double cycle = std::fmod(base_val / 8.0, 1.0);
    if (cycle < 0.0) cycle += 1.0;
    if (colorize_gradient_palette_bgr(cycle, palette, b, g, r)) return;

    switch (palette) {
        case Colormap::HsvWheel: {
            // cycle = 1440 steps; k=180 → one full HSV cycle per 8 halvings of x
            const int idx = std::max(0, static_cast<int>(180.0 * base_val));
            const double h = static_cast<double>(idx % 1440) / 4.0;
            int rr = 0, gg = 0, bb = 0;
            hsv_to_rgb(h, 1.0, 1.0, rr, gg, bb);
            r = static_cast<uint8_t>(rr);
            g = static_cast<uint8_t>(gg);
            b = static_cast<uint8_t>(bb);
            return;
        }
        case Colormap::Tri765: {
            // cycle = 765 steps; k=96 → one tri cycle per ~8 halvings of x
            const int idx = std::max(0, static_cast<int>(96.0 * base_val));
            const int m    = idx % 765;
            const int band = m / 255;
            const int d    = m % 255;
            int rr = 255, gg = 255, bb = 255;
            if      (band == 0) { rr = 255 - d; gg = d;       bb = 255;     }
            else if (band == 1) { rr = d;       gg = 255;     bb = 255 - d; }
            else                { rr = 255;     gg = 255 - d; bb = d;       }
            r = static_cast<uint8_t>(clamp255(rr));
            g = static_cast<uint8_t>(clamp255(gg));
            b = static_cast<uint8_t>(clamp255(bb));
            return;
        }
        case Colormap::HsRainbow: {
            // Direct log-scale indexing into 6-band rainbow, no cycling.
            // idx = int((2 - log2(x)) * 35), clamped to [0, 1785]
            const int idx = std::max(0, std::min(1785, static_cast<int>(35.0 * base_val)));
            rainbow_from_index(idx, b, g, r);
            return;
        }
        default:   // Grayscale (and any other palette)
        case Colormap::Grayscale: {
            // cycle = 256 steps; k=32 → one gray cycle per 8 halvings of x
            const int idx = std::max(0, static_cast<int>(32.0 * base_val));
            r = g = b = static_cast<uint8_t>(idx % 256);
            return;
        }
    }
}

// Colorize a scalar field value in [0,1]. Used for HS-style metric output
// (min_abs, max_abs, min_pairwise_dist). Writes BGR.
// Uses SINGLE-CYCLE formulas — the high-frequency escape-time formula (53/27/139)
// is not appropriate here and would produce dozens of rings across [0,1].
inline void colorize_field_bgr(
    double v01, Colormap palette,
    uint8_t& b, uint8_t& g, uint8_t& r
) {
    if (v01 < 0.0) v01 = 0.0;
    if (v01 > 1.0) v01 = 1.0;
    constexpr double PI = 3.141592653589793;
    if (colorize_gradient_palette_bgr(v01, palette, b, g, r)) return;

    switch (palette) {
        case Colormap::Grayscale: {
            const int v = clamp255(static_cast<int>(v01 * 255.0));
            r = g = b = static_cast<uint8_t>(v);
            return;
        }
        case Colormap::HsvWheel: {
            int rr = 0, gg = 0, bb = 0;
            hsv_to_rgb(v01 * 360.0, 1.0, 1.0, rr, gg, bb);
            r = static_cast<uint8_t>(rr);
            g = static_cast<uint8_t>(gg);
            b = static_cast<uint8_t>(bb);
            return;
        }
        case Colormap::Tri765: {
            const int m    = static_cast<int>(v01 * 765.0);
            const int band = (m / 255) % 3;
            const int d    = m % 255;
            int rr = 255, gg = 255, bb = 255;
            if      (band == 0) { rr = 255 - d; gg = d;       bb = 255;     }
            else if (band == 1) { rr = d;       gg = 255;     bb = 255 - d; }
            else                { rr = 255;     gg = 255 - d; bb = d;       }
            r = static_cast<uint8_t>(clamp255(rr));
            g = static_cast<uint8_t>(clamp255(gg));
            b = static_cast<uint8_t>(clamp255(bb));
            return;
        }
        case Colormap::Mod17: {
            // 17 discrete gray steps across [0,1]
            const int idx = std::min(16, static_cast<int>(v01 * 17.0));
            r = g = b = static_cast<uint8_t>(idx * 15);
            return;
        }
        case Colormap::HsRainbow: {
            // Map v01 ∈ [0,1] to the 6-band rainbow index [0, 1785].
            const int idx = std::min(1785, static_cast<int>(v01 * 1785.0));
            rainbow_from_index(idx, b, g, r);
            return;
        }
        case Colormap::ClassicCos:
        default: {
            // Three-phase cosine, one full cycle across [0,1] (not 53 cycles).
            r = static_cast<uint8_t>(clamp255(static_cast<int>(128.0 - 128.0 * std::cos(v01 * 2.0 * PI))));
            g = static_cast<uint8_t>(clamp255(static_cast<int>(128.0 - 128.0 * std::cos(v01 * 2.0 * PI + 2.094395))));
            b = static_cast<uint8_t>(clamp255(static_cast<int>(128.0 - 128.0 * std::cos(v01 * 2.0 * PI + 4.188790))));
            return;
        }
    }
}

} // namespace fsd::compute
