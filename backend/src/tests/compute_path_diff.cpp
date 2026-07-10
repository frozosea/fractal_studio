#include "compute/engine_select.hpp"
#include "compute/ln_map.hpp"
#include "compute/map_kernel.hpp"
#include "compute/colorize.hpp"
#include "compute/perturbation.hpp"
#include "compute/transition_kernel.hpp"

#if defined(FSD_HAS_MPFR)
#  include <mpfr.h>
#endif

#include <opencv2/core.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using fsd::compute::Colormap;
using fsd::compute::FieldOutput;
using fsd::compute::MapParams;
using fsd::compute::MapStats;
using fsd::compute::Metric;
using fsd::compute::TransitionParams;
using fsd::compute::Variant;

struct DiffLimits {
    int max_channel = 32;
    double mean_abs = 1.0;
    double bad_pixel_ratio = 0.02;
    int bad_pixel_threshold = 12;
};

struct DiffStats {
    int max_channel = 0;
    double mean_abs = 0.0;
    double bad_pixel_ratio = 0.0;
    int bad_pixels = 0;
    int pixel_count = 0;
};

struct RenderScene {
    std::string name;
    MapParams params;
    bool compare_engines = true;
    bool expect_fx64 = true;
};

struct Rendered {
    cv::Mat image;
    MapStats stats;
};

struct FieldRendered {
    FieldOutput field;
    MapStats stats;
};

struct TransitionScene {
    std::string name;
    TransitionParams params;
};

struct Runner {
    int compared = 0;
    int skipped = 0;
    int failed = 0;
    std::set<std::string> seen_engines;
    std::set<std::string> seen_scalars;
};

bool env_enabled(const char* name) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return false;
    std::string v(raw);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return !(v == "0" || v == "false" || v == "no" || v == "off");
}

std::string expected_scalar_name(const std::string& requested) {
    if (requested == "double" || requested == "float64") return "fp64";
    if (requested == "float" || requested == "float32") return "fp32";
    if (requested == "long_double" || requested == "longdouble" || requested == "ldouble") return "fp80";
    if (requested == "float128" || requested == "__float128" ||
        requested == "quad" || requested == "binary128") return "fp128";
    if (requested == "q6.57" || requested == "q657" || requested == "fixed57") return "fx64";
    if (requested == "q459" || requested == "fx59" || requested == "fixed59") return "q4.59";
    if (requested == "q360" || requested == "fx60" || requested == "fixed60") return "q3.60";
    return requested;
}

bool scalar_matches(const std::string& requested, const std::string& actual) {
    return expected_scalar_name(requested) == actual;
}

// Perturbation reports "perturb-<ref>-<delta>", where <ref> depends on the
// depth tier ("fp128", "mpfr192", ...). These scenes expect fp64 deltas.
bool perturb_fp64_scalar(const std::string& actual) {
    const std::string suffix = "-fp64";
    return actual.rfind("perturb-", 0) == 0 &&
           actual.size() > suffix.size() &&
           actual.compare(actual.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string variant_slug(Variant variant) {
    return fsd::compute::variant_name(variant);
}

std::vector<Variant> mandelbrot_family_variants() {
    return {
        Variant::Mandelbrot,
        Variant::Tri,
        Variant::Boat,
        Variant::Duck,
        Variant::Bell,
        Variant::Fish,
        Variant::Vase,
        Variant::Bird,
        Variant::Mask,
        Variant::Ship,
    };
}

MapParams base_params() {
    MapParams p;
    p.width = 160;
    p.height = 120;
    p.iterations = 220;
    p.bailout = 2.0;
    p.bailout_sq = 4.0;
    p.colormap = Colormap::ClassicCos;
    p.render_threads = 0;
    return p;
}

std::vector<RenderScene> quick_scenes() {
    std::vector<RenderScene> scenes;

    {
        MapParams p = base_params();
        p.center_re = -0.75;
        p.center_im = 0.0;
        p.scale = 2.6;
        p.variant = Variant::Mandelbrot;
        p.metric = Metric::Escape;
        scenes.push_back({"mandelbrot_escape", p});
    }

    {
        MapParams p = base_params();
        p.center_re = -0.72;
        p.center_im = 0.08;
        p.scale = 2.35;
        p.rotation_deg = 37.0;
        p.variant = Variant::Mandelbrot;
        p.metric = Metric::Escape;
        p.colormap = Colormap::Mod17;
        scenes.push_back({"mandelbrot_escape_rotated", p});
    }

    {
        MapParams p = base_params();
        p.center_re = -0.3;
        p.center_im = 0.0;
        p.scale = 3.0;
        p.iterations = 200;
        p.variant = Variant::Tri;
        p.metric = Metric::Escape;
        p.colormap = Colormap::Tri765;
        scenes.push_back({"tricorn_escape_tri765", p});
    }

    {
        MapParams p = base_params();
        p.center_re = -0.75;
        p.center_im = 0.0;
        p.scale = 3.0;
        p.iterations = 220;
        p.variant = Variant::Mandelbrot;
        p.metric = Metric::Escape;
        p.colormap = Colormap::Spectral1530;
        scenes.push_back({"mandelbrot_escape_spectral1530", p});
    }

    {
        MapParams p = base_params();
        p.center_re = 0.0;
        p.center_im = 0.0;
        p.scale = 3.0;
        p.iterations = 180;
        p.variant = Variant::Mandelbrot;
        p.metric = Metric::Escape;
        p.julia = true;
        p.julia_re = -0.8;
        p.julia_im = 0.156;
        p.colormap = Colormap::Mod17;
        scenes.push_back({"julia_escape", p});
    }

    {
        MapParams p = base_params();
        p.center_re = -0.75;
        p.center_im = 0.0;
        p.scale = 2.6;
        p.iterations = 180;
        p.variant = Variant::Mandelbrot;
        p.metric = Metric::Escape;
        p.colormap = Colormap::HsRainbow;
        scenes.push_back({"mandelbrot_escape_hs_rainbow_fallback", p});
    }

    {
        MapParams p = base_params();
        p.center_re = -0.45;
        p.center_im = -0.55;
        p.scale = 2.4;
        p.iterations = 180;
        p.variant = Variant::Boat;
        p.metric = Metric::MinAbs;
        p.colormap = Colormap::HsvWheel;
        scenes.push_back({"hs_burning_ship_min_abs_hsv", p});
    }

    {
        MapParams p = base_params();
        p.center_re = -0.2;
        p.center_im = 0.0;
        p.scale = 2.8;
        p.iterations = 160;
        p.variant = Variant::Fish;
        p.metric = Metric::MaxAbs;
        p.colormap = Colormap::Grayscale;
        scenes.push_back({"hs_buffalo_max_abs_gray", p});
    }

    {
        MapParams p = base_params();
        p.center_re = -0.3;
        p.center_im = -0.25;
        p.scale = 2.7;
        p.iterations = 160;
        p.variant = Variant::Vase;
        p.metric = Metric::Envelope;
        p.colormap = Colormap::Tri765;
        scenes.push_back({"hs_perp_buffalo_envelope_tri765", p});
    }

    {
        MapParams p = base_params();
        p.center_re = -0.55;
        p.center_im = 0.0;
        p.scale = 2.8;
        p.iterations = 150;
        p.variant = Variant::Mandelbrot;
        p.metric = Metric::MinAbs;
        p.colormap = Colormap::HsRainbow;
        scenes.push_back({"hs_mandelbrot_min_abs_rainbow", p});
    }

    {
        MapParams p = base_params();
        p.center_re = 0.0;
        p.center_im = 0.0;
        p.scale = 2.7;
        p.iterations = 150;
        p.variant = Variant::Bird;
        p.metric = Metric::Envelope;
        p.colormap = Colormap::ClassicCos;
        p.julia = true;
        p.julia_re = -0.125;
        p.julia_im = 0.75;
        scenes.push_back({"hs_julia_celtic_ship_envelope", p});
    }

    {
        MapParams p = base_params();
        p.width = 96;
        p.height = 72;
        p.center_re = -0.65;
        p.center_im = 0.0;
        p.scale = 2.6;
        p.iterations = 72;
        p.variant = Variant::Mandelbrot;
        p.metric = Metric::MinPairwiseDist;
        p.pairwise_cap = 32;
        p.colormap = Colormap::HsRainbow;
        scenes.push_back({"hs_mandelbrot_pairwise_rainbow", p, false, false});
    }

    {
        MapParams p = base_params();
        p.width = 96;
        p.height = 72;
        p.center_re = -0.45;
        p.center_im = -0.55;
        p.scale = 2.4;
        p.iterations = 120;
        p.variant = Variant::Boat;
        p.metric = Metric::MinAbs;
        p.colormap = Colormap::HsvWheel;
        p.smooth = true;
        scenes.push_back({"hs_burning_ship_min_abs_smooth", p, false, false});
    }

    {
        MapParams p = base_params();
        p.width = 96;
        p.height = 72;
        p.center_re = 0.0;
        p.center_im = 0.0;
        p.scale = 3.0;
        p.iterations = 80;
        p.bailout = 64.0;
        p.bailout_sq = 4096.0;
        p.variant = Variant::SinZ;
        p.metric = Metric::Escape;
        p.colormap = Colormap::Grayscale;
        scenes.push_back({"sin_z_escape_scalar_fallback", p, false, false});
    }

    return scenes;
}

std::vector<RenderScene> mandelbrot_variant_matrix_scenes() {
    std::vector<RenderScene> scenes;
    for (Variant variant : mandelbrot_family_variants()) {
        MapParams p = base_params();
        p.width = 96;
        p.height = 72;
        p.iterations = 96;
        p.center_re = -0.35;
        p.center_im = 0.0;
        p.scale = 3.0;
        p.variant = variant;
        p.metric = Metric::Escape;
        p.colormap = Colormap::Mod17;
        scenes.push_back({"variant_escape_" + variant_slug(variant), p});
    }
    return scenes;
}

std::vector<RenderScene> julia_variant_matrix_scenes() {
    std::vector<RenderScene> scenes;
    for (Variant variant : mandelbrot_family_variants()) {
        MapParams p = base_params();
        p.width = 96;
        p.height = 72;
        p.iterations = 96;
        p.center_re = 0.0;
        p.center_im = 0.0;
        p.scale = 3.0;
        p.variant = variant;
        p.metric = Metric::Escape;
        p.colormap = Colormap::Mod17;
        p.julia = true;
        p.julia_re = -0.25;
        p.julia_im = 0.5;
        scenes.push_back({"julia_variant_escape_" + variant_slug(variant), p});
    }
    return scenes;
}

std::vector<RenderScene> fp32_equivalence_scenes() {
    std::vector<RenderScene> scenes;

    {
        MapParams p;
        p.center_re = 0.0;
        p.center_im = 0.0;
        p.scale = 4.0;
        p.width = 8;
        p.height = 8;
        p.iterations = 4;
        p.bailout = 2.0;
        p.bailout_sq = 4.0;
        p.variant = Variant::Mandelbrot;
        p.metric = Metric::Escape;
        p.colormap = Colormap::Mod17;
        scenes.push_back({"fp32_equiv_mandelbrot_grid", p});
    }

    {
        MapParams p;
        p.center_re = 0.0;
        p.center_im = 0.0;
        p.scale = 2.0;
        p.width = 8;
        p.height = 8;
        p.iterations = 5;
        p.bailout = 2.0;
        p.bailout_sq = 4.0;
        p.variant = Variant::Mandelbrot;
        p.metric = Metric::Escape;
        p.colormap = Colormap::Mod17;
        p.julia = true;
        p.julia_re = -0.5;
        p.julia_im = 0.0;
        scenes.push_back({"fp32_equiv_julia_grid", p});
    }

    return scenes;
}

std::vector<RenderScene> fp64_fx64_equivalence_scenes() {
    std::vector<RenderScene> scenes;

    {
        MapParams p;
        p.center_re = 0.0;
        p.center_im = 0.0;
        p.scale = 4.0;
        p.width = 8;
        p.height = 8;
        p.iterations = 4;
        p.bailout = 2.0;
        p.bailout_sq = 4.0;
        p.variant = Variant::Mandelbrot;
        p.metric = Metric::Escape;
        p.colormap = Colormap::Mod17;
        scenes.push_back({"fp64_fx64_equiv_escape_grid", p});
    }

    {
        MapParams p;
        p.center_re = 0.0;
        p.center_im = 0.0;
        p.scale = 2.0;
        p.width = 8;
        p.height = 8;
        p.iterations = 3;
        p.bailout = 2.0;
        p.bailout_sq = 4.0;
        p.variant = Variant::Mandelbrot;
        p.metric = Metric::MinAbs;
        p.colormap = Colormap::Grayscale;
        scenes.push_back({"fp64_fx64_equiv_min_abs_grid", p});
    }

    {
        MapParams p;
        p.center_re = 0.0;
        p.center_im = 0.0;
        p.scale = 2.0;
        p.width = 8;
        p.height = 8;
        p.iterations = 3;
        p.bailout = 2.0;
        p.bailout_sq = 4.0;
        p.variant = Variant::Mandelbrot;
        p.metric = Metric::MaxAbs;
        p.colormap = Colormap::Grayscale;
        scenes.push_back({"fp64_fx64_equiv_max_abs_grid", p});
    }

    {
        MapParams p;
        p.center_re = 0.0;
        p.center_im = 0.0;
        p.scale = 2.0;
        p.width = 8;
        p.height = 8;
        p.iterations = 3;
        p.bailout = 2.0;
        p.bailout_sq = 4.0;
        p.variant = Variant::Mandelbrot;
        p.metric = Metric::Envelope;
        p.colormap = Colormap::Grayscale;
        scenes.push_back({"fp64_fx64_equiv_envelope_grid", p});
    }

    return scenes;
}

RenderScene slow_hybrid_scene() {
    MapParams p;
    p.center_re = -0.75;
    p.center_im = 0.0;
    p.scale = 2.6;
    p.width = 1024;
    p.height = 900;
    p.iterations = 660;
    p.bailout = 2.0;
    p.bailout_sq = 4.0;
    p.variant = Variant::Mandelbrot;
    p.metric = Metric::Escape;
    p.colormap = Colormap::ClassicCos;
    return {"slow_hybrid_escape", p};
}

TransitionParams base_transition_params() {
    TransitionParams p;
    p.base.center_re = -0.45;
    p.base.center_im = 0.0;
    p.base.scale = 3.0;
    p.base.width = 96;
    p.base.height = 72;
    p.base.iterations = 96;
    p.base.bailout = 2.0;
    p.base.bailout_sq = 4.0;
    p.from_variant = Variant::Mandelbrot;
    p.to_variant = Variant::Boat;
    p.base.metric = Metric::Escape;
    p.base.colormap = Colormap::Mod17;
    p.base.render_threads = 0;
    p.theta_milli_deg_set = true;
    return p;
}

std::vector<TransitionScene> transition_direct_scenes() {
    std::vector<TransitionScene> scenes;
    for (Variant variant : mandelbrot_family_variants()) {
        {
            TransitionParams p = base_transition_params();
            p.from_variant = variant;
            p.to_variant = Variant::Boat;
            p.theta_milli_deg = 0;
            scenes.push_back({"transition_from_slice_" + variant_slug(variant), p});
        }
        {
            TransitionParams p = base_transition_params();
            p.from_variant = Variant::Mandelbrot;
            p.to_variant = variant;
            p.theta_milli_deg = 90 * 1000;
            scenes.push_back({"transition_to_slice_" + variant_slug(variant), p});
        }
    }
    return scenes;
}

std::vector<TransitionScene> transition_smoke_scenes() {
    std::vector<TransitionScene> scenes;

    {
        TransitionParams p = base_transition_params();
        p.theta_milli_deg = 45 * 1000;
        p.base.rotation_deg = 31.0;
        p.from_variant = Variant::Mandelbrot;
        p.to_variant = Variant::Boat;
        p.base.metric = Metric::Escape;
        p.base.colormap = Colormap::Mod17;
        scenes.push_back({"transition_mid_escape_mandelbrot_to_ship", p});
    }

    {
        TransitionParams p = base_transition_params();
        p.theta_milli_deg = 30 * 1000;
        p.from_variant = Variant::Tri;
        p.to_variant = Variant::Bird;
        p.base.metric = Metric::Envelope;
        p.base.colormap = Colormap::HsRainbow;
        scenes.push_back({"transition_mid_envelope_rainbow", p});
    }

    {
        TransitionParams p = base_transition_params();
        p.base.width = 72;
        p.base.height = 56;
        p.base.iterations = 64;
        p.theta_milli_deg = 60 * 1000;
        p.from_variant = Variant::Fish;
        p.to_variant = Variant::Ship;
        p.base.metric = Metric::MinPairwiseDist;
        p.base.pairwise_cap = 24;
        p.base.colormap = Colormap::Grayscale;
        scenes.push_back({"transition_mid_pairwise_gray", p});
    }

    {
        TransitionParams p = base_transition_params();
        p.theta_milli_deg = -45 * 1000;
        p.from_variant = Variant::Duck;
        p.to_variant = Variant::Mask;
        p.base.metric = Metric::MinAbs;
        p.base.colormap = Colormap::HsvWheel;
        p.base.smooth = true;
        scenes.push_back({"transition_mid_min_abs_smooth", p});
    }

    return scenes;
}

Rendered render_scene(const RenderScene& scene, const std::string& engine, const std::string& scalar) {
    MapParams p = scene.params;
    p.engine = engine;
    p.scalar_type = scalar;
    cv::Mat image;
    MapStats stats = fsd::compute::render_map(p, image);
    return {image, stats};
}

Rendered render_transition_scene(const TransitionScene& scene, const std::string& engine, const std::string& scalar) {
    TransitionParams p = scene.params;
    p.base.engine = engine;
    p.base.scalar_type = scalar;
    cv::Mat image;
    MapStats stats = fsd::compute::render_transition(p, image);
    return {image, stats};
}

FieldRendered render_field_scene(const RenderScene& scene, const std::string& scalar,
                                 const std::string& engine = "openmp") {
    MapParams p = scene.params;
    p.engine = engine;
    p.scalar_type = scalar;
    FieldOutput field;
    MapStats stats = fsd::compute::render_map_field(p, field);
    return {field, stats};
}

DiffStats diff_images(const cv::Mat& a, const cv::Mat& b, int bad_threshold) {
    if (a.empty() || b.empty() || a.rows != b.rows || a.cols != b.cols || a.type() != b.type()) {
        throw std::runtime_error("image shape mismatch");
    }
    if (a.type() != CV_8UC3) {
        throw std::runtime_error("expected CV_8UC3 images");
    }

    DiffStats stats;
    stats.pixel_count = a.rows * a.cols;
    long long sum = 0;

    for (int y = 0; y < a.rows; ++y) {
        const unsigned char* pa = a.ptr<unsigned char>(y);
        const unsigned char* pb = b.ptr<unsigned char>(y);
        for (int x = 0; x < a.cols; ++x) {
            bool bad_pixel = false;
            for (int c = 0; c < 3; ++c) {
                const int d = std::abs(static_cast<int>(pa[3 * x + c]) - static_cast<int>(pb[3 * x + c]));
                stats.max_channel = std::max(stats.max_channel, d);
                sum += d;
                if (d > bad_threshold) bad_pixel = true;
            }
            if (bad_pixel) ++stats.bad_pixels;
        }
    }

    stats.mean_abs = static_cast<double>(sum) / static_cast<double>(std::max(1, stats.pixel_count * 3));
    stats.bad_pixel_ratio = static_cast<double>(stats.bad_pixels) / static_cast<double>(std::max(1, stats.pixel_count));
    return stats;
}

DiffStats diff_field_outputs(const FieldOutput& a, const FieldOutput& b, double bad_threshold) {
    if (a.width != b.width || a.height != b.height || a.metric != b.metric) {
        throw std::runtime_error("field shape mismatch");
    }

    DiffStats stats;
    stats.pixel_count = a.width * a.height;

    double sum = 0.0;
    if (a.metric == Metric::Escape) {
        if (a.iter_u32.size() != b.iter_u32.size() || a.norm_f32.size() != b.norm_f32.size()) {
            throw std::runtime_error("escape field buffer mismatch");
        }
        for (size_t i = 0; i < a.iter_u32.size(); ++i) {
            const double iter_delta = std::fabs(
                static_cast<double>(a.iter_u32[i]) - static_cast<double>(b.iter_u32[i]));
            const double norm_delta = std::fabs(
                static_cast<double>(a.norm_f32[i]) - static_cast<double>(b.norm_f32[i]));
            const double d = std::max(iter_delta, norm_delta);
            stats.max_channel = std::max(stats.max_channel, static_cast<int>(std::ceil(d)));
            sum += d;
            if (d > bad_threshold) ++stats.bad_pixels;
        }
    } else {
        if (a.field_f64.size() != b.field_f64.size()) {
            throw std::runtime_error("field buffer mismatch");
        }
        for (size_t i = 0; i < a.field_f64.size(); ++i) {
            const double av = a.field_f64[i];
            const double bv = b.field_f64[i];
            const double d = (std::isfinite(av) && std::isfinite(bv))
                ? std::fabs(av - bv)
                : (av == bv ? 0.0 : std::numeric_limits<double>::infinity());
            const double finite_d = std::isfinite(d) ? d : bad_threshold + 1.0;
            stats.max_channel = std::max(stats.max_channel, static_cast<int>(std::ceil(finite_d)));
            sum += finite_d;
            if (finite_d > bad_threshold) ++stats.bad_pixels;
        }
    }

    stats.mean_abs = sum / static_cast<double>(std::max(1, stats.pixel_count));
    stats.bad_pixel_ratio = static_cast<double>(stats.bad_pixels) / static_cast<double>(std::max(1, stats.pixel_count));
    return stats;
}

bool within_limits(const DiffStats& stats, const DiffLimits& limits) {
    return stats.max_channel <= limits.max_channel &&
           stats.mean_abs <= limits.mean_abs &&
           stats.bad_pixel_ratio <= limits.bad_pixel_ratio;
}

void remember(Runner& runner, const Rendered& rendered) {
    runner.seen_engines.insert(rendered.stats.engine_used);
    runner.seen_scalars.insert(rendered.stats.scalar_used);
}

void remember_stats(Runner& runner, const MapStats& stats) {
    runner.seen_engines.insert(stats.engine_used);
    runner.seen_scalars.insert(stats.scalar_used);
}

void require_actual_or_fail(
    Runner& runner,
    const std::string& context,
    const Rendered& rendered,
    const std::string& engine,
    const std::string& scalar
) {
    bool ok = rendered.stats.engine_used == engine && scalar_matches(scalar, rendered.stats.scalar_used);
    if (!ok) {
        ++runner.failed;
        std::cerr << "[FAIL] " << context
                  << " requested=" << engine << "/" << scalar
                  << " actual=" << rendered.stats.engine_used << "/" << rendered.stats.scalar_used
                  << " (required baseline path fell back)\n";
    }
}

void require_actual_stats_or_fail(
    Runner& runner,
    const std::string& context,
    const MapStats& stats,
    const std::string& engine,
    const std::string& scalar
) {
    bool ok = stats.engine_used == engine && scalar_matches(scalar, stats.scalar_used);
    if (!ok) {
        ++runner.failed;
        std::cerr << "[FAIL] " << context
                  << " requested=" << engine << "/" << scalar
                  << " actual=" << stats.engine_used << "/" << stats.scalar_used
                  << " (required baseline path fell back)\n";
    }
}

void compare_required(
    Runner& runner,
    const RenderScene& scene,
    const Rendered& baseline,
    const std::string& engine,
    const std::string& scalar,
    const DiffLimits& limits
) {
    const std::string label = scene.name + " " + engine + "/" + scalar;
    Rendered candidate;
    try {
        candidate = render_scene(scene, engine, scalar);
    } catch (const std::exception& ex) {
        ++runner.failed;
        std::cerr << "[FAIL] " << label << " threw: " << ex.what() << "\n";
        return;
    }
    remember(runner, candidate);

    if (candidate.stats.engine_used != engine || !scalar_matches(scalar, candidate.stats.scalar_used)) {
        ++runner.skipped;
        std::cout << "[SKIP] " << label
                  << " actual=" << candidate.stats.engine_used << "/" << candidate.stats.scalar_used
                  << " elapsed_ms=" << std::fixed << std::setprecision(3) << candidate.stats.elapsed_ms
                  << "\n";
        return;
    }

    const DiffStats stats = diff_images(baseline.image, candidate.image, limits.bad_pixel_threshold);
    ++runner.compared;
    const bool ok = within_limits(stats, limits);
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label
              << " actual=" << candidate.stats.engine_used << "/" << candidate.stats.scalar_used
              << " max=" << stats.max_channel
              << " mean=" << std::fixed << std::setprecision(4) << stats.mean_abs
              << " bad=" << std::setprecision(4) << stats.bad_pixel_ratio
              << " elapsed_ms=" << std::setprecision(3) << candidate.stats.elapsed_ms
              << "\n";
    if (!ok) ++runner.failed;
}

void compare_scalar_to_fp32(
    Runner& runner,
    const RenderScene& scene,
    const Rendered& fp32_baseline,
    const std::string& scalar,
    const DiffLimits& limits
) {
    const std::string label = scene.name + " scalar openmp/" + scalar + " vs openmp/fp32-equivalent";
    Rendered candidate;
    try {
        candidate = render_scene(scene, "openmp", scalar);
    } catch (const std::exception& ex) {
        ++runner.failed;
        std::cerr << "[FAIL] " << label << " threw: " << ex.what() << "\n";
        return;
    }
    remember(runner, candidate);
    require_actual_or_fail(runner, label, candidate, "openmp", scalar);
    if (candidate.stats.engine_used != "openmp" || !scalar_matches(scalar, candidate.stats.scalar_used)) {
        return;
    }

    const DiffStats stats = diff_images(fp32_baseline.image, candidate.image, limits.bad_pixel_threshold);
    ++runner.compared;
    const bool ok = within_limits(stats, limits);
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label
              << " actual=" << candidate.stats.engine_used << "/" << candidate.stats.scalar_used
              << " max=" << stats.max_channel
              << " mean=" << std::fixed << std::setprecision(4) << stats.mean_abs
              << " bad=" << std::setprecision(4) << stats.bad_pixel_ratio
              << " elapsed_ms=" << std::setprecision(3) << candidate.stats.elapsed_ms
              << "\n";
    if (!ok) ++runner.failed;
}

void compare_scalar_pair(
    Runner& runner,
    const RenderScene& scene,
    const std::string& baseline_scalar,
    const std::string& candidate_scalar,
    const DiffLimits& limits
) {
    const std::string label = scene.name + " scalar openmp/" + candidate_scalar
        + " vs openmp/" + baseline_scalar;
    Rendered baseline;
    Rendered candidate;
    try {
        baseline = render_scene(scene, "openmp", baseline_scalar);
        candidate = render_scene(scene, "openmp", candidate_scalar);
    } catch (const std::exception& ex) {
        ++runner.failed;
        std::cerr << "[FAIL] " << label << " threw: " << ex.what() << "\n";
        return;
    }
    remember(runner, baseline);
    remember(runner, candidate);
    require_actual_or_fail(runner, label + " baseline", baseline, "openmp", baseline_scalar);
    require_actual_or_fail(runner, label + " candidate", candidate, "openmp", candidate_scalar);
    if (baseline.stats.engine_used != "openmp" || !scalar_matches(baseline_scalar, baseline.stats.scalar_used) ||
        candidate.stats.engine_used != "openmp" || !scalar_matches(candidate_scalar, candidate.stats.scalar_used)) {
        return;
    }

    const DiffStats stats = diff_images(baseline.image, candidate.image, limits.bad_pixel_threshold);
    ++runner.compared;
    const bool ok = within_limits(stats, limits);
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label
              << " max=" << stats.max_channel
              << " mean=" << std::fixed << std::setprecision(4) << stats.mean_abs
              << " bad=" << std::setprecision(4) << stats.bad_pixel_ratio
              << " elapsed_ms=" << std::setprecision(3) << candidate.stats.elapsed_ms
              << "\n";
    if (!ok) ++runner.failed;
}

void compare_field_scalar_pair(
    Runner& runner,
    const RenderScene& scene,
    const std::string& baseline_scalar,
    const std::string& candidate_scalar,
    const DiffLimits& limits
) {
    const std::string label = scene.name + " raw-field openmp/" + candidate_scalar
        + " vs openmp/" + baseline_scalar;
    FieldRendered baseline;
    FieldRendered candidate;
    try {
        baseline = render_field_scene(scene, baseline_scalar);
        candidate = render_field_scene(scene, candidate_scalar);
    } catch (const std::exception& ex) {
        ++runner.failed;
        std::cerr << "[FAIL] " << label << " threw: " << ex.what() << "\n";
        return;
    }
    remember_stats(runner, baseline.stats);
    remember_stats(runner, candidate.stats);
    require_actual_stats_or_fail(runner, label + " baseline", baseline.stats, "openmp", baseline_scalar);
    require_actual_stats_or_fail(runner, label + " candidate", candidate.stats, "openmp", candidate_scalar);
    if (baseline.stats.engine_used != "openmp" || !scalar_matches(baseline_scalar, baseline.stats.scalar_used) ||
        candidate.stats.engine_used != "openmp" || !scalar_matches(candidate_scalar, candidate.stats.scalar_used)) {
        return;
    }

    const DiffStats stats = diff_field_outputs(baseline.field, candidate.field, limits.bad_pixel_threshold);
    ++runner.compared;
    const bool ok = within_limits(stats, limits);
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label
              << " max=" << stats.max_channel
              << " mean=" << std::fixed << std::setprecision(8) << stats.mean_abs
              << " bad=" << std::setprecision(4) << stats.bad_pixel_ratio
              << " elapsed_ms=" << std::setprecision(3) << candidate.stats.elapsed_ms
              << "\n";
    if (!ok) ++runner.failed;
}

// Escape FIELD parity across engines: the fast SIMD/CUDA field path (iter_u32 + norm_f32)
// must match the scalar OpenMP field that feeds equalized coloring. A systematic off-by-one
// in the iteration convention blows past the limit; a handful of boundary float-rounding
// differences (FMA/order) do not. Skips silently when the engine is unavailable at runtime.
void compare_field_engine_pair(
    Runner& runner,
    const RenderScene& scene,
    const std::string& engine,
    const std::string& scalar,
    const DiffLimits& limits
) {
    const std::string label = scene.name + " raw-field " + engine + "/" + scalar
        + " vs openmp/" + scalar;
    FieldRendered baseline;
    FieldRendered candidate;
    try {
        baseline = render_field_scene(scene, scalar, "openmp");
        candidate = render_field_scene(scene, scalar, engine);
    } catch (const std::exception& ex) {
        ++runner.failed;
        std::cerr << "[FAIL] " << label << " threw: " << ex.what() << "\n";
        return;
    }
    remember_stats(runner, baseline.stats);
    remember_stats(runner, candidate.stats);
    if (baseline.stats.engine_used != "openmp") return;
    if (candidate.stats.engine_used != engine) {
        std::cout << "[INFO] " << label << " skipped (engine_used="
                  << candidate.stats.engine_used << ")\n";
        return;
    }

    const DiffStats stats = diff_field_outputs(baseline.field, candidate.field, limits.bad_pixel_threshold);
    ++runner.compared;
    const bool ok = within_limits(stats, limits);
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label
              << " max=" << stats.max_channel
              << " mean=" << std::fixed << std::setprecision(8) << stats.mean_abs
              << " bad=" << std::setprecision(4) << stats.bad_pixel_ratio
              << " elapsed_ms=" << std::setprecision(3) << candidate.stats.elapsed_ms
              << "\n";
    if (!ok) ++runner.failed;
}

int count_unique_bgr(const cv::Mat& image) {
    if (image.empty() || image.type() != CV_8UC3) return 0;
    std::set<int> colors;
    for (int y = 0; y < image.rows; ++y) {
        const uint8_t* row = image.ptr<uint8_t>(y);
        for (int x = 0; x < image.cols; ++x) {
            const int key = static_cast<int>(row[3 * x])
                          | (static_cast<int>(row[3 * x + 1]) << 8)
                          | (static_cast<int>(row[3 * x + 2]) << 16);
            colors.insert(key);
        }
    }
    return static_cast<int>(colors.size());
}

void check_mandelbrot_compare_overlay(Runner& runner) {
    {
        MapParams deep = base_params();
        deep.width = 16;
        deep.height = 12;
        deep.iterations = 512;
        deep.center_re_str =
            "-1.76526611720061270724703428790405209320896702624611953902327263";
        deep.center_im_str =
            "0.01321543629471747625471748763193155388170530590908552047994656";
        deep.center_re = std::stod(deep.center_re_str);
        deep.center_im = std::stod(deep.center_im_str);
        deep.scale = 1.439943461486e-43;
        deep.variant = Variant::Mandelbrot;
        deep.metric = Metric::MandelShipAgree;
        deep.scalar_type = "auto";

        const bool ok = !fsd::compute::perturbation_applicable(deep);
        ++runner.compared;
        std::cout << (ok ? "[PASS] " : "[FAIL] ")
                  << "mandel_ship_agree excludes perturbation\n";
        if (!ok) ++runner.failed;
    }

    RenderScene scene;
    scene.name = "mandel_ship_agree_overlay";
    scene.params = base_params();
    scene.params.width = 72;
    scene.params.height = 54;
    scene.params.iterations = 96;
    scene.params.center_re = -0.45;
    scene.params.center_im = -0.55;
    scene.params.scale = 2.4;
    scene.params.variant = Variant::Boat;
    scene.params.metric = Metric::MandelShipAgree;
    scene.params.colormap = Colormap::Mod17;
    scene.compare_engines = false;
    scene.expect_fx64 = false;

    Rendered overlay;
    FieldRendered raw_field;
    cv::Mat raw_colored;
    try {
        overlay = render_scene(scene, "openmp", "fp64");
        raw_field = render_field_scene(scene, "fp64", "openmp");
        raw_colored = fsd::compute::colorize_direct(scene.params, raw_field.field);
    } catch (const std::exception& ex) {
        ++runner.failed;
        std::cerr << "[FAIL] " << scene.name << " threw: " << ex.what() << "\n";
        return;
    }
    remember(runner, overlay);
    remember_stats(runner, raw_field.stats);
    require_actual_or_fail(runner, scene.name + " overlay", overlay, "openmp", "fp64");
    require_actual_stats_or_fail(runner, scene.name + " raw field", raw_field.stats, "openmp", "fp64");
    if (overlay.stats.engine_used != "openmp" || !scalar_matches("fp64", overlay.stats.scalar_used) ||
        raw_field.stats.engine_used != "openmp" || !scalar_matches("fp64", raw_field.stats.scalar_used)) {
        return;
    }

    int agreed = 0;
    int diverged = 0;
    for (double v : raw_field.field.field_f64) {
        if (v >= 0.5) ++agreed;
        else ++diverged;
    }
    const int overlay_colors = count_unique_bgr(overlay.image);
    const int raw_colors = count_unique_bgr(raw_colored);
    const DiffStats diff = diff_images(overlay.image, raw_colored, 0);
    const bool ok =
        overlay.image.type() == CV_8UC3 &&
        overlay_colors > 16 &&
        raw_colors <= 2 &&
        agreed > 0 &&
        diverged > 0 &&
        diff.bad_pixel_ratio > 0.20;

    ++runner.compared;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << scene.name
              << " overlay_colors=" << overlay_colors
              << " raw_colors=" << raw_colors
              << " agreed=" << agreed
              << " diverged=" << diverged
              << " overlay_vs_raw_bad=" << std::fixed << std::setprecision(4)
              << diff.bad_pixel_ratio
              << " elapsed_ms=" << std::setprecision(3) << overlay.stats.elapsed_ms
              << "\n";
    if (!ok) ++runner.failed;
}

// Add one unit at the 10^-place decimal position of a signed decimal string
// (magnitude increment with carry). Used to probe ground-truth conditioning.
std::string shift_decimal_at(const std::string& s, int place) {
    const size_t dot = s.find('.');
    if (dot == std::string::npos || place < 1) return s;
    std::string out = s;
    size_t pos = dot + static_cast<size_t>(place);
    while (out.size() <= pos) out.push_back('0');
    for (;;) {
        if (out[pos] == '.') { --pos; continue; }
        if (out[pos] != '9') { ++out[pos]; break; }
        out[pos] = '0';
        if (pos == 0 || out[pos - 1] == '-') break;  // clamp; never happens for our coords
        --pos;
    }
    return out;
}

// Compare the perturbation renderer against direct fp128 iteration on a deep
// scene. Coordinates are decimal strings (parsed at full precision by the
// reference-orbit builders); fp128 direct iteration stays a valid ground
// truth down to scale ~1e-30.
//
// Escape-iteration counts are not everywhere a well-conditioned function of
// the pixel position: near iteration-band boundaries (dense around minibrot
// halos) a sub-pixel nudge legitimately changes counts by thousands, which is
// also the scale of fp64 perturbation's accuracy contract (each pixel is the
// true value of a point within a small fraction of a pixel). The comparison
// therefore renders fp128 twice — at the scene center and with the center
// shifted diagonally by ~0.1 pixel — and scores only pixels whose ground
// truth is self-consistent between the two.
void compare_perturbation_scene_to_fp128(
    Runner& runner, const std::string& name,
    const char* cre_str, const char* cim_str, double scale, int iterations,
    bool julia = false, double julia_re = 0.0, double julia_im = 0.0,
    double rotation_deg = 0.0) {
#if defined(FSD_HAS_FLOAT128)
    RenderScene scene;
    scene.name = name;
    scene.params = base_params();
    scene.params.width = 24;
    scene.params.height = 18;
    scene.params.iterations = iterations;
    scene.params.center_re_str = cre_str;
    scene.params.center_im_str = cim_str;
    scene.params.center_re = std::stod(scene.params.center_re_str);
    scene.params.center_im = std::stod(scene.params.center_im_str);
    scene.params.scale = scale;
    scene.params.variant = Variant::Mandelbrot;
    scene.params.metric = Metric::Escape;
    scene.params.colormap = Colormap::Spectral1530;
    scene.params.julia = julia;
    scene.params.julia_re = julia_re;
    scene.params.julia_im = julia_im;
    scene.params.rotation_deg = rotation_deg;

    RenderScene probe = scene;                 // conditioning probe (~0.1 px diagonal)
    const int probe_place = static_cast<int>(std::ceil(
        -std::log10(scale / scene.params.height))) + 1;
    probe.params.center_re_str = shift_decimal_at(scene.params.center_re_str, probe_place);
    probe.params.center_im_str = shift_decimal_at(scene.params.center_im_str, probe_place);

    FieldRendered perturb;
    FieldRendered reference;
    FieldRendered shifted;
    try {
        perturb = render_field_scene(scene, "auto", "openmp");
        reference = render_field_scene(scene, "fp128", "openmp");
        shifted = render_field_scene(probe, "fp128", "openmp");
    } catch (const std::exception& ex) {
        ++runner.failed;
        std::cerr << "[FAIL] " << scene.name << " threw: " << ex.what() << "\n";
        return;
    }
    remember_stats(runner, perturb.stats);
    remember_stats(runner, reference.stats);

    if (!perturb_fp64_scalar(perturb.stats.scalar_used) ||
        reference.stats.engine_used != "openmp" ||
        !scalar_matches("fp128", reference.stats.scalar_used)) {
        ++runner.failed;
        std::cerr << "[FAIL] " << scene.name
                  << " actual=" << perturb.stats.engine_used << "/" << perturb.stats.scalar_used
                  << " reference=" << reference.stats.engine_used << "/" << reference.stats.scalar_used
                  << "\n";
        return;
    }

    const auto& a = perturb.field.iter_u32;
    const auto& b = reference.field.iter_u32;
    const auto& c = shifted.field.iter_u32;
    if (a.size() != b.size() || a.size() != c.size() || a.empty()) {
        ++runner.failed;
        std::cerr << "[FAIL] " << scene.name << " field size mismatch\n";
        return;
    }

    constexpr int allowed_iter_delta = 2;
    struct MaskedScore {
        int max_delta = 0;
        double mean = 0.0;
        double bad_ratio = 1.0;
        double stable_ratio = 0.0;
    };
    auto score_vs_truth = [&](const std::vector<uint32_t>& cand) {
        MaskedScore s;
        int bad = 0;
        size_t stable = 0;
        long long sum_delta = 0;
        for (size_t i = 0; i < cand.size(); ++i) {
            const int truth_wobble =
                std::abs(static_cast<int>(b[i]) - static_cast<int>(c[i]));
            if (truth_wobble > allowed_iter_delta) continue;  // ill-conditioned pixel
            ++stable;
            const int delta = std::abs(static_cast<int>(cand[i]) - static_cast<int>(b[i]));
            s.max_delta = std::max(s.max_delta, delta);
            sum_delta += delta;
            if (delta > allowed_iter_delta) ++bad;
        }
        s.stable_ratio = static_cast<double>(stable) / static_cast<double>(cand.size());
        if (stable) {
            s.mean = static_cast<double>(sum_delta) / static_cast<double>(stable);
            s.bad_ratio = static_cast<double>(bad) / static_cast<double>(stable);
        }
        return s;
    };
    auto report = [&](const std::string& label, const MaskedScore& s, double elapsed_ms) {
        const bool ok = s.stable_ratio >= 0.50 && s.mean <= 1.00 && s.bad_ratio <= 0.02;
        ++runner.compared;
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << scene.name
                  << " " << label
                  << " max_iter_delta=" << s.max_delta
                  << " mean=" << std::fixed << std::setprecision(4) << s.mean
                  << " bad=" << std::setprecision(4) << s.bad_ratio
                  << " stable=" << std::setprecision(4) << s.stable_ratio
                  << " elapsed_ms=" << std::setprecision(3) << elapsed_ms
                  << "\n";
        if (!ok) ++runner.failed;
    };

    report("perturbation vs fp128", score_vs_truth(a), perturb.stats.elapsed_ms);

    // Engine parity: the AVX2/AVX-512/CUDA batch kernels must match the same
    // ground truth on the same conditioning mask. Skipped when the engine is
    // unavailable at runtime (the render falls back and reports another name).
    for (const char* eng : {"avx2", "avx512", "cuda"}) {
        FieldRendered alt;
        try {
            alt = render_field_scene(scene, "auto", eng);
        } catch (const std::exception& ex) {
            ++runner.failed;
            std::cerr << "[FAIL] " << scene.name << " engine " << eng
                      << " threw: " << ex.what() << "\n";
            continue;
        }
        if (alt.stats.engine_used != eng) {
            ++runner.skipped;
            continue;
        }
        remember_stats(runner, alt.stats);
        if (!perturb_fp64_scalar(alt.stats.scalar_used) ||
            alt.field.iter_u32.size() != b.size()) {
            ++runner.failed;
            std::cerr << "[FAIL] " << scene.name << " engine " << eng
                      << " scalar=" << alt.stats.scalar_used << "\n";
            continue;
        }
        report(std::string("perturbation vs fp128 (") + eng + ")",
               score_vs_truth(alt.field.iter_u32), alt.stats.elapsed_ms);
    }
#else
    (void)cre_str; (void)cim_str; (void)scale; (void)iterations;
    (void)julia; (void)julia_re; (void)julia_im; (void)rotation_deg;
    ++runner.skipped;
    std::cout << "[SKIP] " << name << " needs FSD_HAS_FLOAT128\n";
#endif
}

#if defined(FSD_HAS_MPFR)
// Direct per-pixel MPFR iteration — the ground truth beyond fp128's ~34
// significant digits (deep centers carry far more). Same
// step-then-check escape convention as the render kernels.
void mpfr_direct_field(const MapParams& p, std::vector<uint32_t>& out) {
    const int W = p.width, H = p.height;
    const int bits = std::max(128, static_cast<int>(std::ceil(-std::log2(p.scale))) + 64);
    out.assign(static_cast<size_t>(W) * H, 0u);
    const double span_im = p.scale;
    const double span_re = p.scale * static_cast<double>(W) / H;

    #pragma omp parallel for schedule(dynamic)
    for (int idx = 0; idx < W * H; ++idx) {
        const int x = idx % W, y = idx / W;
        const double off_re = span_re * ((x + 0.5) / W - 0.5);
        const double off_im = -span_im * ((y + 0.5) / H - 0.5);

        mpfr_t cr, ci, zr, zi, zr2, zi2, mag, tmp;
        mpfr_inits2(bits, cr, ci, zr, zi, zr2, zi2, mag, tmp, static_cast<mpfr_ptr>(nullptr));
        mpfr_set_str(cr, p.center_re_str.c_str(), 10, MPFR_RNDN);
        mpfr_add_d(cr, cr, off_re, MPFR_RNDN);
        mpfr_set_str(ci, p.center_im_str.c_str(), 10, MPFR_RNDN);
        mpfr_add_d(ci, ci, off_im, MPFR_RNDN);
        mpfr_set_d(zr, 0.0, MPFR_RNDN);
        mpfr_set_d(zi, 0.0, MPFR_RNDN);

        int it = p.iterations;
        for (int n = 0; n < p.iterations; ++n) {
            mpfr_mul(zr2, zr, zr, MPFR_RNDN);
            mpfr_mul(zi2, zi, zi, MPFR_RNDN);
            mpfr_sub(tmp, zr2, zi2, MPFR_RNDN);
            mpfr_add(tmp, tmp, cr, MPFR_RNDN);
            mpfr_mul(zi, zr, zi, MPFR_RNDN);
            mpfr_mul_ui(zi, zi, 2, MPFR_RNDN);
            mpfr_add(zi, zi, ci, MPFR_RNDN);
            mpfr_set(zr, tmp, MPFR_RNDN);
            mpfr_mul(zr2, zr, zr, MPFR_RNDN);
            mpfr_mul(zi2, zi, zi, MPFR_RNDN);
            mpfr_add(mag, zr2, zi2, MPFR_RNDN);
            if (mpfr_cmp_d(mag, p.bailout_sq) > 0) { it = n; break; }
        }
        out[idx] = static_cast<uint32_t>(it);
        mpfr_clears(cr, ci, zr, zi, zr2, zi2, mag, tmp, static_cast<mpfr_ptr>(nullptr));
    }
}
#endif // FSD_HAS_MPFR

// Perturbation near the practical depth floor (scale is a double, so pixel
// offsets stay normal down to ~1e-306): 1.8e-301 against an MPFR oracle.
// Coordinates mined by MPFR boundary descent (escapes 35k..36k, structured).
void compare_perturbation_1e301_to_mpfr(Runner& runner) {
#if defined(FSD_HAS_MPFR)
    constexpr const char* kDeepRe =
        "-1.7470322863217475556160559269920680483993998877346704308252622068895883308"
        "0623199091839741513162397878542014444667155077593447047661529106600323459749"
        "2847219471994154075529179496211095993131201945758384823672278052553385137724"
        "5979330403947280370455813775896738628778224798552663028595059476231384695610"
        "003274438511866656377931559811665482145";
    constexpr const char* kDeepIm =
        "0.00810688832149185309460686083178576433964891867581208066647240721132065319"
        "9581148680281771869927804634535591891848677939093125623857655640970053817778"
        "7878061867510997705195397305803638355895830594516577795276243062528358469962"
        "2877301351951527232076392608417343136463164149062629077715956486930763092311"
        "20173749783703447743250677382084810596";

    RenderScene scene;
    scene.name = "perturb_deep_1p8e301";
    scene.params = base_params();
    scene.params.width = 24;
    scene.params.height = 18;
    scene.params.iterations = 40000;
    scene.params.center_re_str = kDeepRe;
    scene.params.center_im_str = kDeepIm;
    scene.params.center_re = std::stod(scene.params.center_re_str);
    scene.params.center_im = std::stod(scene.params.center_im_str);
    scene.params.scale = 1.829149541201e-301;
    scene.params.variant = Variant::Mandelbrot;
    scene.params.metric = Metric::Escape;
    scene.params.colormap = Colormap::Spectral1530;

    // ~0.01 px conditioning probe: at 35k+ iterations the escape bands are
    // dense enough that a 0.1 px nudge flips half the frame; 0.01 px still
    // sits far above perturbation's own noise scale.
    RenderScene probe = scene;
    const int probe_place = static_cast<int>(std::ceil(
        -std::log10(scene.params.scale / scene.params.height))) + 2;
    probe.params.center_re_str = shift_decimal_at(scene.params.center_re_str, probe_place);
    probe.params.center_im_str = shift_decimal_at(scene.params.center_im_str, probe_place);

    FieldRendered perturb;
    std::vector<uint32_t> b, c;
    try {
        perturb = render_field_scene(scene, "auto", "openmp");
        mpfr_direct_field(scene.params, b);
        mpfr_direct_field(probe.params, c);
    } catch (const std::exception& ex) {
        ++runner.failed;
        std::cerr << "[FAIL] " << scene.name << " threw: " << ex.what() << "\n";
        return;
    }
    remember_stats(runner, perturb.stats);

    const auto& a = perturb.field.iter_u32;
    if (!perturb_fp64_scalar(perturb.stats.scalar_used) ||
        a.size() != b.size() || a.size() != c.size() || a.empty()) {
        ++runner.failed;
        std::cerr << "[FAIL] " << scene.name << " scalar="
                  << perturb.stats.scalar_used << " size=" << a.size() << "\n";
        return;
    }

    constexpr int allowed_iter_delta = 2;
    int max_delta = 0, bad = 0;
    size_t stable = 0;
    long long sum_delta = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const int wobble = std::abs(static_cast<int>(b[i]) - static_cast<int>(c[i]));
        if (wobble > allowed_iter_delta) continue;
        ++stable;
        const int delta = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
        max_delta = std::max(max_delta, delta);
        sum_delta += delta;
        if (delta > allowed_iter_delta) ++bad;
    }
    const double stable_ratio = static_cast<double>(stable) / static_cast<double>(a.size());
    const double mean = stable ? static_cast<double>(sum_delta) / static_cast<double>(stable) : 0.0;
    const double bad_ratio = stable ? static_cast<double>(bad) / static_cast<double>(stable) : 1.0;
    const bool ok = stable_ratio >= 0.50 && mean <= 1.00 && bad_ratio <= 0.02;
    ++runner.compared;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << scene.name
              << " perturbation vs mpfr"
              << " max_iter_delta=" << max_delta
              << " mean=" << std::fixed << std::setprecision(4) << mean
              << " bad=" << std::setprecision(4) << bad_ratio
              << " stable=" << std::setprecision(4) << stable_ratio
              << " elapsed_ms=" << std::setprecision(3) << perturb.stats.elapsed_ms
              << "\n";
    if (!ok) ++runner.failed;
#else
    ++runner.skipped;
    std::cout << "[SKIP] perturb_deep_1p8e301 needs FSD_HAS_MPFR\n";
#endif
}

// Non-escape metric under perturbation: the Envelope field (min/max |z|
// extrema) against direct fp128, on the same ~0.1 px conditioning mask.
void compare_perturbation_envelope_to_fp128(Runner& runner) {
#if defined(FSD_HAS_FLOAT128)
    RenderScene scene;
    scene.name = "perturb_envelope_1p8e20";
    scene.params = base_params();
    scene.params.width = 24;
    scene.params.height = 18;
    scene.params.iterations = 4096;
    scene.params.center_re_str = "-1.747032286321747555615588653338769259";
    scene.params.center_im_str = "0.008106888321491853096767791107692308";
    scene.params.center_re = std::stod(scene.params.center_re_str);
    scene.params.center_im = std::stod(scene.params.center_im_str);
    scene.params.scale = 1.829149541201e-20;
    scene.params.variant = Variant::Mandelbrot;
    scene.params.metric = Metric::Envelope;
    scene.params.colormap = Colormap::Spectral1530;

    RenderScene probe = scene;
    const int probe_place = static_cast<int>(std::ceil(
        -std::log10(scene.params.scale / scene.params.height))) + 1;
    probe.params.center_re_str = shift_decimal_at(scene.params.center_re_str, probe_place);
    probe.params.center_im_str = shift_decimal_at(scene.params.center_im_str, probe_place);

    FieldRendered perturb, reference, shifted;
    try {
        perturb = render_field_scene(scene, "auto", "openmp");
        reference = render_field_scene(scene, "fp128", "openmp");
        shifted = render_field_scene(probe, "fp128", "openmp");
    } catch (const std::exception& ex) {
        ++runner.failed;
        std::cerr << "[FAIL] " << scene.name << " threw: " << ex.what() << "\n";
        return;
    }
    remember_stats(runner, perturb.stats);
    remember_stats(runner, reference.stats);

    const auto& a = perturb.field.field_f64;
    const auto& b = reference.field.field_f64;
    const auto& c = shifted.field.field_f64;
    if (!perturb_fp64_scalar(perturb.stats.scalar_used) ||
        a.size() != b.size() || a.size() != c.size() || a.empty()) {
        ++runner.failed;
        std::cerr << "[FAIL] " << scene.name << " scalar="
                  << perturb.stats.scalar_used << " size=" << a.size() << "\n";
        return;
    }

    // The envelope value varies continuously with c (unlike escape counts),
    // so a pixel's ground truth legitimately drifts under the 0.1 px probe
    // shift. Perturbation passes when it lands closer to the truth than that
    // reposition: per-pixel allowance = probe drift + small floor.
    int bad = 0;
    double max_err = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double allowance = 1e-9 + 1e-6 * std::abs(b[i]) + std::abs(b[i] - c[i]);
        const double err = std::abs(a[i] - b[i]);
        max_err = std::max(max_err, err);
        if (err > allowance) ++bad;
    }
    const double bad_ratio = static_cast<double>(bad) / static_cast<double>(a.size());
    const bool ok = bad_ratio <= 0.02;
    ++runner.compared;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << scene.name
              << " envelope vs fp128 max_err=" << std::scientific << std::setprecision(3) << max_err
              << std::fixed
              << " bad=" << std::setprecision(4) << bad_ratio
              << " elapsed_ms=" << std::setprecision(3) << perturb.stats.elapsed_ms
              << "\n";
    if (!ok) ++runner.failed;
#else
    ++runner.skipped;
    std::cout << "[SKIP] perturb_envelope_1p8e20 needs FSD_HAS_FLOAT128\n";
#endif
}

// Deep ln-map strip crossing the fp128→MPFR reference-precision tier at
// kRefFp128MinScale = 1e-26 (the video route derives depthOctaves from
// targetScale with no 80-octave cap, so 112-octave viewpoints produce strips
// whose innermost radius needs the MPFR reference). Renders escape and hist_eq strips at ~119 total octaves and
// checks the deep rows carry structure (not a flat/black band).
void verify_deep_lnmap_strip(Runner& runner, const std::string& engine) {
#if defined(FSD_HAS_FLOAT128) && defined(FSD_HAS_MPFR)
    fsd::compute::LnMapParams lp;
    lp.center_re_str =
        "-1.7470322863217475556160559269920680483993998877346704308252622068895883308"
        "0623199091839741513162397878542014444667155077593447047661529106600323459749"
        "2847219471994154075529179496211095993131201945758384823672278052553385137724"
        "5979330403947280370455813775896738628778224798552663028595059476231384695610"
        "003274438511866656377931559811665482145";
    lp.center_im_str =
        "0.00810688832149185309460686083178576433964891867581208066647240721132065319"
        "9581148680281771869927804634535591891848677939093125623857655640970053817778"
        "7878061867510997705195397305803638355895830594516577795276243062528358469962"
        "2877301351951527232076392608417343136463164149062629077715956486930763092311"
        "20173749783703447743250677382084810596";
    lp.center_re = std::stod(lp.center_re_str);
    lp.center_im = std::stod(lp.center_im_str);
    lp.width_s = 128;
    lp.height_t = static_cast<int>(std::ceil(
        119.0 * 0.6931471805599453 / 6.283185307179586 * lp.width_s));
    lp.iterations = 8192;
    lp.variant = Variant::Mandelbrot;
    lp.colormap = Colormap::Spectral1530;
    lp.engine = engine;

    for (const char* mode : {"escape", "hist_eq"}) {
        lp.color_mode = mode;
        cv::Mat strip;
        fsd::compute::LnMapStats stats;
        try {
            stats = fsd::compute::render_ln_map(lp, strip);
        } catch (const std::exception& ex) {
            ++runner.failed;
            std::cerr << "[FAIL] lnmap_deep_119oct_" << mode << "_" << engine
                      << " threw: " << ex.what() << "\n";
            continue;
        }
        // Deep fifth of the strip (well below the 1e-26 crossing at ~88 octaves).
        const int y0 = lp.height_t * 4 / 5;
        cv::Mat deep = strip.rowRange(y0, lp.height_t);
        double lum_sum = 0.0;
        size_t dark = 0;
        for (int y = 0; y < deep.rows; ++y) {
            const uint8_t* row = deep.ptr<uint8_t>(y);
            for (int x = 0; x < deep.cols; ++x) {
                const int m = std::max({row[3 * x], row[3 * x + 1], row[3 * x + 2]});
                lum_sum += m;
                if (m < 8) ++dark;
            }
        }
        const double dark_ratio = static_cast<double>(dark) /
            (static_cast<double>(deep.rows) * deep.cols);
        const bool perturb = stats.scalar_used.find("perturbation") != std::string::npos;
        const bool ok = perturb && dark_ratio < 0.90;
        ++runner.compared;
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << "lnmap_deep_119oct_" << mode
                  << "_" << engine
                  << " engine=" << stats.engine_used
                  << " scalar=" << stats.scalar_used
                  << " deep_dark=" << std::fixed << std::setprecision(4) << dark_ratio
                  << " elapsed_ms=" << std::setprecision(3) << stats.elapsed_ms
                  << "\n";
        if (!ok) ++runner.failed;
        runner.seen_engines.insert(stats.engine_used);
        runner.seen_scalars.insert(stats.scalar_used);
    }
#else
    (void)engine;
    ++runner.skipped;
    std::cout << "[SKIP] lnmap_deep_119oct needs FSD_HAS_FLOAT128 + FSD_HAS_MPFR\n";
#endif
}

// Deep-zoom perturbation scenes. Coordinates mined by boundary descent so
// every frame keeps an iteration spread; all verified against direct fp128.
void compare_deep_perturbation_scenes(Runner& runner) {
    // Existing shallow-end scene (just past the 1e-13 activation gate).
    compare_perturbation_scene_to_fp128(runner,
        "deep_mandelbrot_perturbation",
        "-1.747032286321746345",
        "0.008106888321491234",
        1.829149541201e-15, 4096);

    // Deep filament at 1.8e-30: exercises the fp128-precision center parse
    // near the bottom of the __float128 tier with fast-escaping dynamics.
    // (Minibrot halos with long shadowing — e.g. the period-3 island antenna
    // at -1.76672314773504240768…+0.00775302006722118372…i, 2e-20 — are NOT
    // usable as comparison anchors: escape counts there change by thousands
    // under a 0.1-pixel nudge of the ground truth itself, so no per-pixel
    // assertion is well-defined. Cancellation rebases are covered here and in
    // every other scene: hundreds to thousands fire per frame.)
    compare_perturbation_scene_to_fp128(runner,
        "perturb_filament_1p8e30",
        "-1.747032286321747555616055926991316089",
        "0.008106888321491853094606860832336525",
        1.829149541201e-30, 8192);

    // Filament view whose center escapes early with slower neighbors:
    // isolates the truncated-reference rebase with a short orbit.
    compare_perturbation_scene_to_fp128(runner,
        "perturb_escaping_ref_1p8e20",
        "-1.747032286321747555615588653338769259",
        "0.008106888321491853096767791107692308",
        1.829149541201e-20, 4096);

    // Same view rotated 30°: covers the rotated-offset path in the scalar,
    // AVX2, and CUDA pixel-offset generation.
    compare_perturbation_scene_to_fp128(runner,
        "perturb_rotated_1p8e20",
        "-1.747032286321747555615588653338769259",
        "0.008106888321491853096767791107692308",
        1.829149541201e-20, 4096,
        /*julia=*/false, 0.0, 0.0, /*rotation_deg=*/30.0);

    // Deep Julia (c inside the main cardioid, quasicircle boundary): the
    // seeded center orbit escapes at ~282 while interior pixels never do,
    // forcing the switch onto the critical orbit of c.
    compare_perturbation_scene_to_fp128(runner,
        "perturb_julia_2e20",
        "-0.369327699081606662507692307692307668",
        "0.602998422270858277092307692307692327",
        2e-20, 4096,
        /*julia=*/true, -0.5, 0.5);
}

MapParams map_params_for_direct_transition(const TransitionParams& p) {
    MapParams mp = p.base;
    mp.variant = p.theta_milli_deg == 90 * 1000 ? p.to_variant : p.from_variant;
    return mp;
}

void compare_transition_direct_to_map(
    Runner& runner,
    const TransitionScene& transition_scene,
    const std::string& scalar,
    const DiffLimits& limits
) {
    RenderScene map_scene{
        transition_scene.name + "_map_reference",
        map_params_for_direct_transition(transition_scene.params),
        false,
        true,
    };
    const std::string label = transition_scene.name + " transition direct vs map openmp/" + scalar;
    Rendered baseline;
    Rendered candidate;
    try {
        baseline = render_scene(map_scene, "openmp", scalar);
        candidate = render_transition_scene(transition_scene, "openmp", scalar);
    } catch (const std::exception& ex) {
        ++runner.failed;
        std::cerr << "[FAIL] " << label << " threw: " << ex.what() << "\n";
        return;
    }
    remember(runner, baseline);
    remember(runner, candidate);
    require_actual_or_fail(runner, label + " map baseline", baseline, "openmp", scalar);
    require_actual_or_fail(runner, label + " transition candidate", candidate, "openmp", scalar);
    if (baseline.stats.engine_used != "openmp" || !scalar_matches(scalar, baseline.stats.scalar_used) ||
        candidate.stats.engine_used != "openmp" || !scalar_matches(scalar, candidate.stats.scalar_used)) {
        return;
    }

    const DiffStats stats = diff_images(baseline.image, candidate.image, limits.bad_pixel_threshold);
    ++runner.compared;
    const bool ok = within_limits(stats, limits);
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label
              << " max=" << stats.max_channel
              << " mean=" << std::fixed << std::setprecision(4) << stats.mean_abs
              << " bad=" << std::setprecision(4) << stats.bad_pixel_ratio
              << " elapsed_ms=" << std::setprecision(3) << candidate.stats.elapsed_ms
              << "\n";
    if (!ok) ++runner.failed;
}

void compare_transition_theta_encoding(
    Runner& runner,
    const TransitionScene& scene,
    const DiffLimits& limits
) {
    constexpr double pi = 3.14159265358979323846264338327950288;
    TransitionScene radians_scene = scene;
    radians_scene.name += "_radians";
    radians_scene.params.theta_milli_deg_set = false;
    radians_scene.params.theta =
        static_cast<double>(scene.params.theta_milli_deg) * pi / (180.0 * 1000.0);

    const std::string label = scene.name + " transition milli-deg vs radians";
    Rendered baseline;
    Rendered candidate;
    try {
        baseline = render_transition_scene(scene, "openmp", "fp64");
        candidate = render_transition_scene(radians_scene, "openmp", "fp64");
    } catch (const std::exception& ex) {
        ++runner.failed;
        std::cerr << "[FAIL] " << label << " threw: " << ex.what() << "\n";
        return;
    }
    remember(runner, baseline);
    remember(runner, candidate);
    require_actual_or_fail(runner, label + " baseline", baseline, "openmp", "fp64");
    require_actual_or_fail(runner, label + " candidate", candidate, "openmp", "fp64");
    if (baseline.stats.engine_used != "openmp" || !scalar_matches("fp64", baseline.stats.scalar_used) ||
        candidate.stats.engine_used != "openmp" || !scalar_matches("fp64", candidate.stats.scalar_used)) {
        return;
    }

    const DiffStats stats = diff_images(baseline.image, candidate.image, limits.bad_pixel_threshold);
    ++runner.compared;
    const bool ok = within_limits(stats, limits);
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label
              << " max=" << stats.max_channel
              << " mean=" << std::fixed << std::setprecision(4) << stats.mean_abs
              << " bad=" << std::setprecision(4) << stats.bad_pixel_ratio
              << " elapsed_ms=" << std::setprecision(3) << candidate.stats.elapsed_ms
              << "\n";
    if (!ok) ++runner.failed;
}

void require_seen(Runner& runner, const std::set<std::string>& seen, const std::string& value, const char* env_name) {
    if (!env_enabled(env_name)) return;
    if (seen.count(value)) return;
    ++runner.failed;
    std::cerr << "[FAIL] required by " << env_name << " but not exercised: " << value << "\n";
}

void require_default_scalar_coverage(Runner& runner, const std::string& scalar) {
    if (runner.seen_scalars.count(scalar)) return;
    ++runner.failed;
    std::cerr << "[FAIL] required default scalar was not exercised: " << scalar << "\n";
}

void print_capabilities() {
    const auto c = fsd::compute::runtime_capabilities();
    std::cout << "[caps]"
              << " openmp=" << c.openmp_compiled << "/" << c.openmp_runtime
              << " avx2=" << c.avx2_compiled << "/" << c.avx2_runtime
              << " fma=" << c.fma_runtime
              << " avx512=" << c.avx512_compiled << "/" << c.avx512_runtime
              << " cuda=" << c.cuda_compiled << "/" << c.cuda_runtime;
    if (c.cuda_runtime) {
        std::cout << " cuda_device=\"" << c.cuda_name << "\""
                  << " sm=" << c.cuda_compute_major << "." << c.cuda_compute_minor;
    }
    std::cout << "\n";
}

} // namespace

int main() {
    Runner runner;
    print_capabilities();

    const DiffLimits same_scalar_limits{255, 2.00, 0.080, 10};
    const DiffLimits exact_scalar_limits{0, 0.0, 0.0, 0};

    std::vector<RenderScene> render_scenes = quick_scenes();
    {
        std::vector<RenderScene> matrix = mandelbrot_variant_matrix_scenes();
        render_scenes.insert(render_scenes.end(), matrix.begin(), matrix.end());
    }
    {
        std::vector<RenderScene> matrix = julia_variant_matrix_scenes();
        render_scenes.insert(render_scenes.end(), matrix.begin(), matrix.end());
    }

    check_mandelbrot_compare_overlay(runner);

    for (const RenderScene& scene : render_scenes) {
        Rendered fp64_baseline = render_scene(scene, "openmp", "fp64");
        remember(runner, fp64_baseline);
        require_actual_or_fail(runner, scene.name + " baseline openmp/fp64", fp64_baseline, "openmp", "fp64");

        Rendered fp32_baseline = render_scene(scene, "openmp", "fp32");
        remember(runner, fp32_baseline);
        require_actual_or_fail(runner, scene.name + " baseline openmp/fp32", fp32_baseline, "openmp", "fp32");

        if (scene.expect_fx64) {
            Rendered fx64_baseline = render_scene(scene, "openmp", "fx64");
            remember(runner, fx64_baseline);
            require_actual_or_fail(runner, scene.name + " baseline openmp/fx64", fx64_baseline, "openmp", "fx64");
            compare_required(runner, scene, fx64_baseline, "cuda", "fx64", same_scalar_limits);
        }

        if (scene.compare_engines) {
            for (const char* engine : {"avx2", "avx512", "cuda"}) {
                compare_required(runner, scene, fp64_baseline, engine, "fp64", same_scalar_limits);
                compare_required(runner, scene, fp32_baseline, engine, "fp32", same_scalar_limits);
            }
            // Escape FIELD parity (feeds equalized coloring). fp64 only — the field path is
            // fp64 by design (the scalar dispatch has no fp32 branch).
            if (scene.params.metric == Metric::Escape) {
                compare_field_engine_pair(runner, scene, "avx512", "fp64", same_scalar_limits);
                compare_field_engine_pair(runner, scene, "avx2", "fp64", same_scalar_limits);
                compare_field_engine_pair(runner, scene, "cuda", "fp64", same_scalar_limits);
            }
        } else {
            std::cout << "[INFO] " << scene.name
                      << " is OpenMP-only for vector/CUDA differential coverage\n";
        }
    }

    for (const TransitionScene& scene : transition_direct_scenes()) {
        compare_transition_direct_to_map(runner, scene, "fp64", exact_scalar_limits);
        compare_transition_direct_to_map(runner, scene, "fp32", exact_scalar_limits);
        compare_transition_direct_to_map(runner, scene, "fx64", exact_scalar_limits);
    }

    for (const TransitionScene& scene : transition_smoke_scenes()) {
        compare_transition_theta_encoding(runner, scene, exact_scalar_limits);
    }

    compare_deep_perturbation_scenes(runner);
    compare_perturbation_envelope_to_fp128(runner);
    compare_perturbation_1e301_to_mpfr(runner);
    verify_deep_lnmap_strip(runner, "openmp");
    verify_deep_lnmap_strip(runner, "auto");

    for (const RenderScene& scene : fp32_equivalence_scenes()) {
        Rendered fp32_baseline = render_scene(scene, "openmp", "fp32");
        remember(runner, fp32_baseline);
        require_actual_or_fail(runner, scene.name + " baseline openmp/fp32-equivalent", fp32_baseline, "openmp", "fp32");

        compare_scalar_to_fp32(runner, scene, fp32_baseline, "fp64", exact_scalar_limits);
        compare_scalar_to_fp32(runner, scene, fp32_baseline, "fx64", exact_scalar_limits);
        compare_scalar_to_fp32(runner, scene, fp32_baseline, "fp80", exact_scalar_limits);
#if defined(FSD_HAS_FLOAT128)
        compare_scalar_to_fp32(runner, scene, fp32_baseline, "fp128", exact_scalar_limits);
#endif
    }

    for (const RenderScene& scene : fp64_fx64_equivalence_scenes()) {
        compare_scalar_pair(runner, scene, "fp64", "fx64", exact_scalar_limits);
        compare_scalar_pair(runner, scene, "fp64", "fp80", exact_scalar_limits);
#if defined(FSD_HAS_FLOAT128)
        compare_scalar_pair(runner, scene, "fp64", "fp128", exact_scalar_limits);
#endif
        if (scene.params.metric != Metric::Escape) {
            compare_field_scalar_pair(runner, scene, "fp64", "fx64", exact_scalar_limits);
            compare_field_scalar_pair(runner, scene, "fp64", "fp80", exact_scalar_limits);
#if defined(FSD_HAS_FLOAT128)
            compare_field_scalar_pair(runner, scene, "fp64", "fp128", exact_scalar_limits);
#endif
        }
    }

    if (env_enabled("FSD_DIFF_INCLUDE_SLOW")) {
        const RenderScene scene = slow_hybrid_scene();
        Rendered cuda_baseline = render_scene(scene, "cuda", "fp64");
        remember(runner, cuda_baseline);
        if (cuda_baseline.stats.engine_used == "cuda" && scalar_matches("fp64", cuda_baseline.stats.scalar_used)) {
            compare_required(runner, scene, cuda_baseline, "hybrid", "fp64", same_scalar_limits);
        } else {
            ++runner.skipped;
            std::cout << "[SKIP] " << scene.name
                      << " hybrid check needs CUDA baseline, actual="
                      << cuda_baseline.stats.engine_used << "/" << cuda_baseline.stats.scalar_used << "\n";
        }
    }

    require_default_scalar_coverage(runner, "fp64");
    require_default_scalar_coverage(runner, "fp32");
    require_default_scalar_coverage(runner, "fx64");

    require_seen(runner, runner.seen_engines, "avx2", "FSD_DIFF_EXPECT_AVX2");
    require_seen(runner, runner.seen_engines, "avx512", "FSD_DIFF_EXPECT_AVX512");
    require_seen(runner, runner.seen_engines, "cuda", "FSD_DIFF_EXPECT_CUDA");
    require_seen(runner, runner.seen_engines, "hybrid", "FSD_DIFF_EXPECT_HYBRID");
    require_seen(runner, runner.seen_scalars, "fp80", "FSD_DIFF_EXPECT_FP80");
    require_seen(runner, runner.seen_scalars, "fp128", "FSD_DIFF_EXPECT_FP128");

    std::cout << "[summary] compared=" << runner.compared
              << " skipped=" << runner.skipped
              << " failed=" << runner.failed
              << " engines=";
    for (const auto& engine : runner.seen_engines) std::cout << engine << ",";
    std::cout << " scalars=";
    for (const auto& scalar : runner.seen_scalars) std::cout << scalar << ",";
    std::cout << "\n";

    return runner.failed == 0 ? 0 : 1;
}
