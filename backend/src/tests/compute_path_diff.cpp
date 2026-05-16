#include "compute/engine_select.hpp"
#include "compute/map_kernel.hpp"

#include <opencv2/core.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using fsd::compute::Colormap;
using fsd::compute::MapParams;
using fsd::compute::MapStats;
using fsd::compute::Metric;
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
};

struct Rendered {
    cv::Mat image;
    MapStats stats;
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
    if (requested == "q6.57" || requested == "q657" || requested == "fixed57") return "fx64";
    if (requested == "q459" || requested == "fx59" || requested == "fixed59") return "q4.59";
    if (requested == "q360" || requested == "fx60" || requested == "fixed60") return "q3.60";
    return requested;
}

bool scalar_matches(const std::string& requested, const std::string& actual) {
    return expected_scalar_name(requested) == actual;
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
        p.center_re = -0.45;
        p.center_im = -0.55;
        p.scale = 2.4;
        p.iterations = 180;
        p.variant = Variant::Boat;
        p.metric = Metric::MinAbs;
        p.colormap = Colormap::HsvWheel;
        scenes.push_back({"burning_ship_min_abs", p});
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

Rendered render_scene(const RenderScene& scene, const std::string& engine, const std::string& scalar) {
    MapParams p = scene.params;
    p.engine = engine;
    p.scalar_type = scalar;
    cv::Mat image;
    MapStats stats = fsd::compute::render_map(p, image);
    return {image, stats};
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

bool within_limits(const DiffStats& stats, const DiffLimits& limits) {
    return stats.max_channel <= limits.max_channel &&
           stats.mean_abs <= limits.mean_abs &&
           stats.bad_pixel_ratio <= limits.bad_pixel_ratio;
}

void remember(Runner& runner, const Rendered& rendered) {
    runner.seen_engines.insert(rendered.stats.engine_used);
    runner.seen_scalars.insert(rendered.stats.scalar_used);
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

    for (const RenderScene& scene : quick_scenes()) {
        Rendered fp64_baseline = render_scene(scene, "openmp", "fp64");
        remember(runner, fp64_baseline);
        require_actual_or_fail(runner, scene.name + " baseline openmp/fp64", fp64_baseline, "openmp", "fp64");

        Rendered fp32_baseline = render_scene(scene, "openmp", "fp32");
        remember(runner, fp32_baseline);
        require_actual_or_fail(runner, scene.name + " baseline openmp/fp32", fp32_baseline, "openmp", "fp32");

        Rendered fx64_baseline = render_scene(scene, "openmp", "fx64");
        remember(runner, fx64_baseline);
        require_actual_or_fail(runner, scene.name + " baseline openmp/fx64", fx64_baseline, "openmp", "fx64");

        for (const char* engine : {"avx2", "avx512", "cuda"}) {
            compare_required(runner, scene, fp64_baseline, engine, "fp64", same_scalar_limits);
            compare_required(runner, scene, fp32_baseline, engine, "fp32", same_scalar_limits);
        }

        compare_required(runner, scene, fx64_baseline, "cuda", "fx64", same_scalar_limits);
    }

    for (const RenderScene& scene : fp32_equivalence_scenes()) {
        Rendered fp32_baseline = render_scene(scene, "openmp", "fp32");
        remember(runner, fp32_baseline);
        require_actual_or_fail(runner, scene.name + " baseline openmp/fp32-equivalent", fp32_baseline, "openmp", "fp32");

        compare_scalar_to_fp32(runner, scene, fp32_baseline, "fp64", exact_scalar_limits);
        compare_scalar_to_fp32(runner, scene, fp32_baseline, "fx64", exact_scalar_limits);
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
