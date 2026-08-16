// wasm colorize core: ports the TypeScript coloring (escapeColor /
// metricColor / fieldColor from local-render-core.ts) for direct color mode.
// Same IEEE order and byte() clamping; parity-tested against the TS core.
// Build:
//   clang --target=wasm32 -O3 -msimd128 -nostdlib -Wl,--no-entry \
//     -Wl,--export=field_core_colorize -Wl,--export-memory \
//     -Wl,--initial-memory=33554432 -o colorize.wasm colorize.cpp
#include <stdint.h>

static int llround_impl(double x) { return (int)(x + 0.5); }

static double fmod_impl(double a, double b) {
    double q = (double)(long long)(a / b);
    return a - q * b;
}

// cos on [0, 2pi) with a Taylor polynomial; byte-level output only needs
// ~1e-6 accuracy.
static double cos_impl(double x) {
    const double two_pi = 6.283185307179586;
    const double pi = 3.141592653589793;
    const double pi_2 = 1.5707963267948966;
    double r = fmod_impl(x, two_pi);
    if (r < 0) r += two_pi;
    int neg = 0;
    if (r > pi) { r = two_pi - r; }
    if (r > pi_2) { r = pi - r; neg = 1; }
    double r2 = r * r;
    double poly = 1.0 + r2 * (-0.49999999999999994
        + r2 * (0.041666666666666664
        + r2 * (-0.001388888888888889
        + r2 * (0.000024801587301587302
        + r2 * (-0.0000002755731922398589
        + r2 * 0.00000000208767569878681)))));
    return neg ? -poly : poly;
}

static inline int byte_val(double value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (int)value;  // JS Math.trunc
}

// gradient tables (inferno/viridis/twilight/ember_blue), same stops as TS
struct Stop { double at; double r, g, b; };
static const Stop INFERNO[] = {
    {0,0,0,4},{.14,31,12,72},{.28,85,15,109},{.42,136,34,106},{.56,186,54,85},
    {.7,227,89,51},{.84,249,140,10},{.94,252,195,55},{1,252,255,164},
};
static const Stop VIRIDIS[] = {{0,68,1,84},{.25,59,82,139},{.5,33,145,140},{.75,94,201,98},{1,253,231,37}};
static const Stop TWILIGHT[] = {
    {0,32,24,70},{.18,63,92,180},{.36,58,150,165},{.54,240,210,120},
    {.72,210,90,90},{.88,90,50,110},{1,32,24,70},
};
static const Stop EMBER_BLUE[] = {{0,5,8,32},{.22,10,70,120},{.48,55,190,185},{.72,245,172,75},{1,255,246,210}};

static int gradient(int palette, double value, uint8_t out[3]) {
    const Stop* stops = nullptr;
    int count = 0;
    switch (palette) {
        case 6: stops = INFERNO; count = 9; break;
        case 7: stops = VIRIDIS; count = 5; break;
        case 8: stops = TWILIGHT; count = 7; break;
        case 9: stops = EMBER_BLUE; count = 5; break;
        default: return 0;
    }
    double t = value < 0 ? 0 : (value > 1 ? 1 : value);
    for (int i = 1; i < count; ++i) {
        if (t > stops[i].at) continue;
        const Stop& left = stops[i - 1];
        const Stop& right = stops[i];
        double denom = right.at - left.at;
        if (denom < 1e-12) denom = 1e-12;
        double u = (t - left.at) / denom;
        out[0] = (uint8_t)llround_impl(left.r * (1 - u) + right.r * u);
        out[1] = (uint8_t)llround_impl(left.g * (1 - u) + right.g * u);
        out[2] = (uint8_t)llround_impl(left.b * (1 - u) + right.b * u);
        return 1;
    }
    const Stop& last = stops[count - 1];
    out[0] = (uint8_t)last.r; out[1] = (uint8_t)last.g; out[2] = (uint8_t)last.b;
    return 1;
}

// hsv(hue degrees) - same as TS
static void hsv(double hue, uint8_t out[3]) {
    double h = fmod_impl(fmod_impl(hue, 360) + 360, 360) / 60.0;
    double x = 1 - __builtin_fabs(fmod_impl(h, 2) - 1);
    double r, g, b;
    if (h < 1) { r = 1; g = x; b = 0; }
    else if (h < 2) { r = x; g = 1; b = 0; }
    else if (h < 3) { r = 0; g = 1; b = x; }
    else if (h < 4) { r = 0; g = x; b = 1; }
    else if (h < 5) { r = x; g = 0; b = 1; }
    else { r = 1; g = 0; b = x; }
    out[0] = (uint8_t)byte_val(r * 255);
    out[1] = (uint8_t)byte_val(g * 255);
    out[2] = (uint8_t)byte_val(b * 255);
}

static void hue1530(double index, uint8_t out[3]) {
    double i = fmod_impl(fmod_impl(index, 1530) + 1530, 1530);
    int segment = (int)__builtin_floor(i / 255);
    int d = (int)i % 255;
    switch (segment) {
        case 0: out[0]=0; out[1]=255; out[2]=(uint8_t)d; break;
        case 1: out[0]=0; out[1]=(uint8_t)(255-d); out[2]=255; break;
        case 2: out[0]=(uint8_t)d; out[1]=0; out[2]=255; break;
        case 3: out[0]=255; out[1]=0; out[2]=(uint8_t)(255-d); break;
        case 4: out[0]=255; out[1]=(uint8_t)d; out[2]=0; break;
        default: out[0]=(uint8_t)(255-d); out[1]=255; out[2]=0; break;
    }
}

static void tri765(double index, uint8_t out[3]) {
    double m = fmod_impl(fmod_impl(index, 765) + 765, 765);
    int band = (int)__builtin_floor(m / 255);
    int d = (int)m % 255;
    if (band == 0) { out[0]=(uint8_t)(255-d); out[1]=(uint8_t)d; out[2]=255; }
    else if (band == 1) { out[0]=(uint8_t)d; out[1]=255; out[2]=(uint8_t)(255-d); }
    else { out[0]=255; out[1]=(uint8_t)(255-d); out[2]=(uint8_t)d; }
}

static void rainbow1785(double index, uint8_t out[3]) {
    double i = index < 0 ? 0 : (index > 1785 ? 1785 : index);
    if (i == 0) { out[0]=0; out[1]=0; out[2]=0; return; }
    if (i == 1785) { out[0]=255; out[1]=255; out[2]=255; return; }
    double blue = i, red = 0, green = 0;
    if (i > 255 && i < 510) { red = i - 255; blue = 510 - i; }
    else if (i > 509 && i < 765) { red = 255; blue = i - 510; }
    else if (i > 764 && i < 1020) { green = i - 765; red = 1020 - i; blue = red; }
    else if (i > 1019 && i < 1275) { green = 255; blue = i - 1020; }
    else if (i > 1274 && i < 1530) { green = 255; red = i - 1275; blue = 1530 - i; }
    else if (i > 1529) { green = 255; red = 255; blue = i - 1530; }
    out[0] = (uint8_t)byte_val(red);
    out[1] = (uint8_t)byte_val(green);
    out[2] = (uint8_t)byte_val(blue);
}

// fieldColor(value, palette) - direct metric coloring
static void field_color(double value, int palette, uint8_t out[3]) {
    double t = value;
    if (!(t <= 1) || t < 0) t = 1;  // !Number.isFinite -> 1; clamp [0,1]
    if (t > 1) t = 1;
    if (gradient(palette, t, out)) return;
    switch (palette) {
        case 4: { int v = byte_val(t * 255); out[0]=out[1]=out[2]=(uint8_t)v; return; }
        case 2: hsv(t * 360, out); return;
        case 3: tri765(t * 765, out); return;
        case 5: rainbow1785(t * 1785, out); return;
        case 10: hue1530(((1529 < t * 1530) ? 1529 : t * 1530), out); return;
        case 1: { int v = __builtin_fmin(16, (int)(t * 17)) * 15; out[0]=out[1]=out[2]=(uint8_t)v; return; }
        default: {
            double tau = 6.283185307179586;
            out[0] = (uint8_t)byte_val(128 - 128 * cos_impl(t * tau));
            out[1] = (uint8_t)byte_val(128 - 128 * cos_impl(t * tau + 2.094395));
            out[2] = (uint8_t)byte_val(128 - 128 * cos_impl(t * tau + 4.18879));
            return;
        }
    }
}

extern "C" void field_core_colorize(
    const uint32_t* iters, const float* norms, const double* fields,
    const double* log2log2norms, const double* log2fields,
    uint32_t count,
    uint32_t iterations, uint32_t metric, uint32_t smooth,
    uint32_t colorMap, double bailout,
    uint8_t* out_rgba
) {
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t rgb[16];
        const uint32_t iter = iters[i];
        const double norm = norms[i];
        const double field = fields[i];
        const double log2log2 = log2log2norms[i];
        const double log2field = log2fields[i];
        if (metric == 0) {
            // escapeColor
            if (iter >= iterations) { out_rgba[i*4]=255; out_rgba[i*4+1]=255; out_rgba[i*4+2]=255; out_rgba[i*4+3]=255; continue; }
            double n = (double)(iter + 1) / (double)(iterations + 2);
            double mu = (double)iter;
            if (smooth && norm > 1) {
                mu = ((0 > (double)iter + 1 - log2log2) ? 0 : (double)iter + 1 - log2log2);
                n = fmod_impl(fmod_impl(mu / 32.0, 1) + 1, 1);
            }
            if (gradient((int)colorMap, n, rgb)) { out_rgba[i*4]=rgb[0]; out_rgba[i*4+1]=rgb[1]; out_rgba[i*4+2]=rgb[2]; out_rgba[i*4+3]=255; continue; }
            switch (colorMap) {
                case 2: if (smooth) hsv(n * 360, rgb); else hsv((double)(iter % 1440) / 4.0, rgb); break;
                case 3: if (smooth) tri765(n * 765, rgb); else tri765((double)iter, rgb); break;
                case 4: { int v = byte_val(n * 255); rgb[0]=rgb[1]=rgb[2]=(uint8_t)v; break; }
                case 10: if (mu < 255) { rgb[0]=0; rgb[1]=(uint8_t)byte_val(mu); rgb[2]=0; } else hue1530(mu - 255, rgb); break;
                case 1: if (smooth) { int v = byte_val((double)((int)mu % 17) * 15); rgb[0]=rgb[1]=rgb[2]=(uint8_t)v; }
                    else { rgb[0]=(uint8_t)(iter % 256); rgb[1]=(uint8_t)(iter / 256); rgb[2]=(uint8_t)byte_val((double)((iter % 17) * 17)); } break;
                default: {
                    rgb[0] = (uint8_t)byte_val(128 - 128 * cos_impl(n * 53 * 3.141592653589793));
                    rgb[1] = (uint8_t)byte_val(128 - 128 * cos_impl(n * 27 * 3.141592653589793));
                    rgb[2] = (uint8_t)byte_val(128 - 128 * cos_impl(n * 139 * 3.141592653589793));
                    break;
                }
            }
            out_rgba[i*4]=rgb[0]; out_rgba[i*4+1]=rgb[1]; out_rgba[i*4+2]=rgb[2]; out_rgba[i*4+3]=255;
        } else {
            // metricColor
            const double raw = field;
            if (colorMap == 5) {
                if (raw <= 0 || !(raw <= 1.7976931348623157e308)) { out_rgba[i*4]=255; out_rgba[i*4+1]=255; out_rgba[i*4+2]=255; out_rgba[i*4+3]=255; continue; }
                rainbow1785((36.0 / 35.0 - log2field) * 35.0, rgb);
                out_rgba[i*4]=rgb[0]; out_rgba[i*4+1]=rgb[1]; out_rgba[i*4+2]=rgb[2]; out_rgba[i*4+3]=255; continue;
            }
            if (smooth) {
                if (raw <= 0) { out_rgba[i*4]=255; out_rgba[i*4+1]=255; out_rgba[i*4+2]=255; out_rgba[i*4+3]=255; continue; }
                double base = 2 - log2field;
                double cycle = fmod_impl(fmod_impl(base / 8.0, 1) + 1, 1);
                if (gradient((int)colorMap, cycle, rgb)) { out_rgba[i*4]=rgb[0]; out_rgba[i*4+1]=rgb[1]; out_rgba[i*4+2]=rgb[2]; out_rgba[i*4+3]=255; continue; }
                switch (colorMap) {
                    case 2: hsv((double)((int)((0 > 180 * base) ? 0 : 180 * base) % 1440) / 4.0, rgb); break;
                    case 3: tri765((double)((0 > (int)(96 * base)) ? 0 : (int)(96 * base)), rgb); break;
                    case 10: hue1530((double)((0 > (int)(191 * base)) ? 0 : (int)(191 * base)), rgb); break;
                    case 4: { int v = (int)((0 > (int)(32 * base)) ? 0 : (int)(32 * base)) % 256; rgb[0]=rgb[1]=rgb[2]=(uint8_t)v; break; }
                    default: field_color(raw / bailout, (int)colorMap, rgb); break;
                }
                out_rgba[i*4]=rgb[0]; out_rgba[i*4+1]=rgb[1]; out_rgba[i*4+2]=rgb[2]; out_rgba[i*4+3]=255;
            } else {
                field_color(raw / bailout, (int)colorMap, rgb);
                out_rgba[i*4]=rgb[0]; out_rgba[i*4+1]=rgb[1]; out_rgba[i*4+2]=rgb[2]; out_rgba[i*4+3]=255;
            }
        }
    }
}
