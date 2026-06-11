// routes_ln.cpp
//
// ln-map strip renderer. Produces a single tall PNG whose pixel columns are
// angles θ ∈ [0, 2π) and pixel rows are log-radii descending from ln(4) (the
// outside of the set) toward ln(4) − rows·2π/s (arbitrarily deep). This is
// the same parameterisation C_mandelbrot/big_png_ln.py uses.
//
// For a given row `r` and column `x`:
//     θ = 2π · x / s
//     k = ln(4) − r · 2π / s
//     c = center + e^k · (cos θ + i sin θ)
//
// A single strip rendered once captures the entire zoom sequence around the
// target `center`. In Phase 2, routes_video.cpp will slide a window down the
// strip and exp-warp it into cartesian video frames — no fractal recompute.

#include "routes.hpp"
#include "routes_common.hpp"

#include "../compute/ln_map.hpp"
#include "../compute/variants.hpp"
#include "../compute/colormap.hpp"
#include "../compute/image_io.hpp"

#include <opencv2/core.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fsd {

namespace {

constexpr double TAU     = 6.283185307179586;
constexpr double PI      = 3.141592653589793;
constexpr double LN_TWO  = 0.6931471805599453;
constexpr double LN_FOUR = 1.3862943611198906;

int roundUpToMultiple(int value, int multiple) {
    if (multiple <= 1) return value;
    const int rem = value % multiple;
    return rem == 0 ? value : value + (multiple - rem);
}

int derivedMinStripWidth(int W, int H) {
    const double diag = std::sqrt(static_cast<double>(W) * static_cast<double>(W)
                                + static_cast<double>(H) * static_cast<double>(H));
    const int minWidth = static_cast<int>(std::ceil(diag * PI));
    return roundUpToMultiple(minWidth, 8);
}

double presetQualityScale(const std::string& preset) {
    if (preset == "draft") return 0.35;
    if (preset == "balanced") return 0.55;
    if (preset == "high") return 0.75;
    if (preset == "full") return 1.0;
    return 0.55;
}

std::string defaultQualityPresetForSize(int W, int H) {
    return (W >= 3840 || H >= 2160) ? "balanced" : "high";
}

uint64_t estimateLnMapBytes(int s, int t) {
    return static_cast<uint64_t>(s) * static_cast<uint64_t>(t) * 3u;
}

} // namespace

std::string lnMapRenderRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& body) {
    (void)repoRoot;
    const Json j = parseJsonBody(body);

    const double cr            = j.value("centerRe", 0.0);
    const double ci            = j.value("centerIm", 0.0);
    const bool   julia         = j.value("julia", false);
    const double jre           = j.value("juliaRe", 0.0);
    const double jim           = j.value("juliaIm", 0.0);
    const int    outW          = j.value("width", 0);
    const int    outH          = j.value("height", 0);
    const double depthOctaves  = j.value("depthOctaves", 40.0);
    const int fullWidthS = (outW > 0 && outH > 0) ? derivedMinStripWidth(outW, outH) : 1920;
    std::string qualityPreset = j.value("qualityPreset", defaultQualityPresetForSize(outW, outH));
    double qualityScale = j.value("qualityScale", presetQualityScale(qualityPreset));
    if (!(qualityScale > 0.0) || qualityScale > 1.0 || !std::isfinite(qualityScale)) {
        throw std::runtime_error("invalid qualityScale (0..1)");
    }
    int s = 0;
    if (j.contains("widthS") && !j["widthS"].is_null()) {
        s = j.value("widthS", fullWidthS);
        qualityPreset = "custom";
        qualityScale = static_cast<double>(s) / std::max(1, fullWidthS);
    } else {
        s = static_cast<int>(std::ceil(static_cast<double>(fullWidthS) * qualityScale));
    }
    s = roundUpToMultiple(std::max(128, s), 8);
    const std::string variantStr  = j.value("variant",  std::string("mandelbrot"));
    const std::string colormapStr = j.value("colorMap", std::string("classic_cos"));
    std::string colorMode = j.value("lnMapColorMode", std::string("escape"));
    if (!j.contains("lnMapColorMode") && j.contains("colorMode") && !j["colorMode"].is_null()) {
        colorMode = j.value("colorMode", colorMode);
    }
    const std::string engine      = j.value("engine",   std::string("auto"));
    std::string precisionMode = j.value("precisionMode", std::string("standard"));
    if (j.contains("lnMapMode") && !j["lnMapMode"].is_null()) {
        precisionMode = j.value("lnMapMode", precisionMode);
    }
    std::string scalarType = j.value("scalarType", std::string("auto"));
    if (j.contains("lnMapScalar") && !j["lnMapScalar"].is_null()) {
        scalarType = j.value("lnMapScalar", scalarType);
    }
    const double fastFp32Depth = j.value("fastFp32DepthOctaves", 18.0);
    const double fastFp64Depth = j.value("fastFp64DepthOctaves", 34.0);
    const bool fastValidate = j.value("fastValidate", true);
    const double fastValidationBandOctaves = j.value("fastValidationBandOctaves", 4.0);
    const int fastValidationSampleRows = j.value("fastValidationSampleRows", 5);
    const int fastValidationSampleCols = j.value("fastValidationSampleCols", 24);
    const double fastValidationMaxMismatchRatio = j.value("fastValidationMaxMismatchRatio", 0.01);
    const int fastValidationMaxP99IterDelta = j.value("fastValidationMaxP99IterDelta", 16);
    const double fastValidationMaxMeanColorDelta = j.value("fastValidationMaxMeanColorDelta", 8.0);
    const int iters            = j.value("iterations", 4096);

    if (s < 128 || s > 65536)              throw std::runtime_error("invalid widthS (128..65536)");
    if (depthOctaves < 1.0 || depthOctaves > 80.0) throw std::runtime_error("invalid depthOctaves (1..80)");
    if (iters < 1 || iters > 10000000)     throw std::runtime_error("invalid iterations");
    if (precisionMode != "standard" && precisionMode != "fast") {
        throw std::runtime_error("invalid precisionMode (standard|fast)");
    }
    if (!compute::ln_map_color_mode_supported(colorMode)) {
        throw std::runtime_error("invalid lnMapColorMode (escape|hist_eq|row_eq|log_lift|bands|frontier)");
    }
    if (!(fastFp32Depth >= 0.0) || !(fastFp64Depth >= 0.0) ||
        !std::isfinite(fastFp32Depth) || !std::isfinite(fastFp64Depth)) {
        throw std::runtime_error("invalid fast depth thresholds");
    }
    if (!(fastValidationBandOctaves > 0.0) || !std::isfinite(fastValidationBandOctaves) ||
        fastValidationSampleRows < 1 || fastValidationSampleRows > 32 ||
        fastValidationSampleCols < 1 || fastValidationSampleCols > 256 ||
        !(fastValidationMaxMismatchRatio >= 0.0) || fastValidationMaxMismatchRatio > 1.0 ||
        fastValidationMaxP99IterDelta < 0 ||
        !(fastValidationMaxMeanColorDelta >= 0.0) || !std::isfinite(fastValidationMaxMeanColorDelta)) {
        throw std::runtime_error("invalid fast validation settings");
    }

    // Same formula as big_png_ln.py:20 — 2 base octaves + requested depth.
    const double t_exact = (2.0 + depthOctaves) * LN_TWO / TAU * static_cast<double>(s);
    const int t = static_cast<int>(std::ceil(t_exact));
    const uint64_t estimatedPeakMemory = estimateLnMapBytes(s, t);

    compute::Variant v;
    if (!compute::variant_from_name(variantStr.c_str(), v)) v = compute::Variant::Mandelbrot;
    double bailout = j.contains("bailout") && !j["bailout"].is_null()
        ? j.value("bailout", 2.0)
        : compute::variant_default_bailout(v);
    const double bailoutSq = j.contains("bailoutSq") && !j["bailoutSq"].is_null()
        ? j.value("bailoutSq", compute::variant_default_bailout_sq(v))
        : (j.contains("bailout") && !j["bailout"].is_null()
            ? bailout * bailout
            : compute::variant_default_bailout_sq(v));
    if (j.contains("bailoutSq") && !j["bailoutSq"].is_null() &&
        !(j.contains("bailout") && !j["bailout"].is_null())) {
        bailout = std::sqrt(bailoutSq);
    }
    if (!(bailout > 0.0) || !std::isfinite(bailout)) throw std::runtime_error("invalid bailout");
    if (!(bailoutSq > 0.0) || !std::isfinite(bailoutSq)) throw std::runtime_error("invalid bailoutSq");
    compute::Colormap cm;
    if (!compute::colormap_from_name(colormapStr.c_str(), cm)) cm = compute::Colormap::ClassicCos;

    auto run = runner.createRun("ln-map", body);
    runner.setStatus(run.id, "running");

    std::string pngPath;
    compute::LnMapStats stats;

    try {
        cv::Mat strip(t, s, CV_8UC3);
        compute::LnMapParams lp;
        lp.julia = julia;
        lp.center_re = cr;
        lp.center_im = ci;
        lp.julia_re = jre;
        lp.julia_im = jim;
        lp.width_s = s;
        lp.height_t = t;
        lp.iterations = iters;
        lp.bailout = bailout;
        lp.bailout_sq = bailoutSq;
        lp.variant = v;
        lp.colormap = cm;
        lp.color_mode = colorMode;
        lp.engine = engine;
        lp.precision_mode = precisionMode;
        lp.scalar_type = scalarType;
        lp.fast_fp32_depth_octaves = fastFp32Depth;
        lp.fast_fp64_depth_octaves = fastFp64Depth;
        lp.fast_validate = fastValidate;
        lp.fast_validation_band_octaves = fastValidationBandOctaves;
        lp.fast_validation_sample_rows = fastValidationSampleRows;
        lp.fast_validation_sample_cols = fastValidationSampleCols;
        lp.fast_validation_max_mismatch_ratio = fastValidationMaxMismatchRatio;
        lp.fast_validation_max_p99_iter_delta = fastValidationMaxP99IterDelta;
        lp.fast_validation_max_mean_color_delta = fastValidationMaxMeanColorDelta;
        stats = compute::render_ln_map(lp, strip);

        const std::filesystem::path stripPath =
            std::filesystem::path(run.outputDir) / "ln_map.png";
        compute::write_png(stripPath.string(), strip);
        pngPath = stripPath.string();
        runner.addArtifact(run.id, Artifact{"ln-map", pngPath, "image"});

        // Sidecar JSON so Phase 2 video export can read the parameters.
        Json sidecar = {
            {"centerRe",     cr},
            {"centerIm",     ci},
            {"julia",        julia},
            {"juliaRe",      jre},
            {"juliaIm",      jim},
            {"widthS",       s},
            {"actualWidthS", s},
            {"fullWidthS",   fullWidthS},
            {"heightT",      t},
            {"depthOctaves", depthOctaves},
            {"qualityPreset", qualityPreset},
            {"qualityScale", qualityScale},
            {"estimatedPeakMemory", estimatedPeakMemory},
            {"lnRadiusTop",  LN_FOUR},
            {"variant",      variantStr},
            {"colorMap",     colormapStr},
            {"lnMapColorMode", colorMode},
            {"iterations",   iters},
            {"bailout",      bailout},
            {"bailoutSq",    bailoutSq},
            {"engine",       stats.engine_used},
            {"scalar",       stats.scalar_used},
            {"precisionMode", stats.precision_mode},
            {"layerSummary", stats.layer_summary},
            {"validationSummary", stats.validation_summary},
        };
        const std::filesystem::path sidecarPath =
            std::filesystem::path(run.outputDir) / "ln_map.json";
        std::filesystem::create_directories(sidecarPath.parent_path());
        {
            std::ofstream os(sidecarPath);
            os << sidecar.dump(2);
        }
        runner.addArtifact(run.id, Artifact{"ln-map", sidecarPath.string(), "report"});
        runner.setStatus(run.id, "completed");
    } catch (const std::exception&) {
        runner.setStatus(run.id, "failed");
        throw;
    }

    const std::string artifactId = run.id + ":ln_map.png";
    Json resp = {
        {"runId",       run.id},
        {"status",      "completed"},
        {"artifactId",  artifactId},
        {"imagePath",   "/api/artifacts/content?artifactId=" + artifactId},
        {"widthS",      s},
        {"actualWidthS", s},
        {"fullWidthS",  fullWidthS},
        {"heightT",     t},
        {"depthOctaves", depthOctaves},
        {"qualityPreset", qualityPreset},
        {"qualityScale", qualityScale},
        {"estimatedPeakMemory", estimatedPeakMemory},
        {"engineUsed",  stats.engine_used},
        {"scalarUsed",  stats.scalar_used},
        {"precisionMode", stats.precision_mode},
        {"lnMapColorMode", colorMode},
        {"layerSummary", stats.layer_summary},
        {"validationSummary", stats.validation_summary},
        {"generatedMs", stats.elapsed_ms},
    };
    return resp.dump();
}

} // namespace fsd
