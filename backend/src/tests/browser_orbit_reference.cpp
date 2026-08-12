#include "compute/variants.hpp"
#include "compute/colormap.hpp"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using fsd::compute::Cx;
using fsd::compute::Variant;

template <Variant V>
void emit_sample(const char* name, double re, double im, bool julia, double jr, double ji, int max_iter) {
    Cx<double> z{julia ? re : 0.0, julia ? im : 0.0};
    const Cx<double> c{julia ? jr : re, julia ? ji : im};
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = 0.0;
    int iteration = max_iter;
    double norm = 0.0;
    const double bailout = fsd::compute::variant_default_bailout(V);
    for (int i = 0; i < max_iter; ++i) {
        z = fsd::compute::variant_step<V, double>(z, c);
        norm = z.re * z.re + z.im * z.im;
        minimum = std::min(minimum, norm);
        maximum = std::max(maximum, norm);
        const bool escaped = !std::isfinite(norm) || (fsd::compute::variant_is_transcendental(V)
            ? std::max(std::abs(z.re), std::abs(z.im)) > bailout : norm > bailout * bailout);
        if (escaped) { iteration = i; break; }
    }
    if (iteration == max_iter) norm = 0.0;
    std::cout << name << '\t' << std::setprecision(17) << re << '\t' << im << '\t'
              << (julia ? 1 : 0) << '\t' << jr << '\t' << ji << '\t' << max_iter << '\t'
              << iteration << '\t' << norm << '\t'
              << (std::isfinite(minimum) ? std::sqrt(minimum) : 0.0) << '\t'
              << (maximum > 0.0 ? std::sqrt(maximum) : 0.0) << '\n';
}

template <Variant V>
void emit_variant(const char* name) {
    emit_sample<V>(name, -0.75, 0.1, false, 0, 0, 320);
    emit_sample<V>(name, 0.31, -0.27, false, 0, 0, 180);
    emit_sample<V>(name, -0.12, 0.72, true, -0.8, 0.156, 240);
}

template <Variant V>
void emit_frame(bool julia, fsd::compute::Colormap palette, bool smooth) {
    constexpr int width = 32, height = 24, max_iter = 180;
    constexpr double center_re = -0.64, center_im = 0.03, scale = 2.4;
    const double bailout = fsd::compute::variant_default_bailout(V);
    std::vector<unsigned char> rgba(static_cast<size_t>(width * height * 4), 255);
    for (int py = 0; py < height; ++py) for (int px = 0; px < width; ++px) {
        const double re = center_re + ((px + 0.5) / width - 0.5) * scale * width / height;
        const double im = center_im + (0.5 - (py + 0.5) / height) * scale;
        Cx<double> z{julia ? re : 0.0, julia ? im : 0.0};
        const Cx<double> c{julia ? -0.8 : re, julia ? 0.156 : im};
        int iteration = max_iter; double norm = 0.0;
        for (int i = 0; i < max_iter; ++i) {
            z = fsd::compute::variant_step<V, double>(z, c); norm = z.re*z.re + z.im*z.im;
            const bool escaped = !std::isfinite(norm) || (fsd::compute::variant_is_transcendental(V)
                ? std::max(std::abs(z.re), std::abs(z.im)) > bailout : norm > bailout*bailout);
            if (escaped) { iteration = i; break; }
        }
        unsigned char b=0,g=0,r=0; fsd::compute::colorize_escape_bgr(iteration, max_iter, palette, norm, smooth, b, g, r);
        const size_t offset = static_cast<size_t>((py * width + px) * 4); rgba[offset]=r; rgba[offset+1]=g; rgba[offset+2]=b;
    }
    std::cout.write(reinterpret_cast<const char*>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--frame") {
        if (std::string(argv[2]) == "mandelbrot") emit_frame<Variant::Mandelbrot>(false, fsd::compute::Colormap::ClassicCos, false);
        else if (std::string(argv[2]) == "burning_ship_julia") emit_frame<Variant::Boat>(true, fsd::compute::Colormap::Viridis, true);
        else return 2;
        return 0;
    }
    emit_variant<Variant::Mandelbrot>("mandelbrot");
    emit_variant<Variant::Tri>("tricorn");
    emit_variant<Variant::Boat>("burning_ship");
    emit_variant<Variant::Duck>("celtic");
    emit_variant<Variant::Bell>("heart");
    emit_variant<Variant::Fish>("buffalo");
    emit_variant<Variant::Vase>("perp_buffalo");
    emit_variant<Variant::Bird>("celtic_ship");
    emit_variant<Variant::Mask>("mandelceltic");
    emit_variant<Variant::Ship>("perp_ship");
    emit_variant<Variant::SinZ>("sin_z");
    emit_variant<Variant::CosZ>("cos_z");
    emit_variant<Variant::ExpZ>("exp_z");
    emit_variant<Variant::SinhZ>("sinh_z");
    emit_variant<Variant::CoshZ>("cosh_z");
    emit_variant<Variant::TanZ>("tan_z");
}
