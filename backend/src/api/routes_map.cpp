// routes_map.cpp
//
// Native map renderer endpoint. All 10 variants × all metrics × optional
// Mandelbrot↔Burning-Ship transition. Dispatches to compute/map_kernel.cpp or
// compute/transition_kernel.cpp. Export requests write PNG artifacts;
// interactive requests can return raw RGBA8 frames without run/artifact I/O.

#include "routes.hpp"
#include "routes_common.hpp"
#include "resource_manager.hpp"

#include "../compute/map_kernel.hpp"
#include "../compute/ln_map.hpp"   // build_map_equalization, colorize_map_field_equalized
#include "../compute/tile_scheduler.hpp"
#include "../compute/transition_kernel.hpp"
#include "../compute/image_io.hpp"
#include "../compute/variants.hpp"
#include "../compute/escape_time.hpp"
#include "../compute/colormap.hpp"

#include <cmath>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace fsd {

namespace {

constexpr double TAU = 6.28318530717958647692528676655900576;
constexpr double PI  = 3.14159265358979323846264338327950288;
constexpr int THETA_SCALE = 1000;
constexpr int THETA_HALF_TURN_MDEG = 180 * THETA_SCALE;
constexpr int THETA_FULL_TURN_MDEG = 360 * THETA_SCALE;

struct InteractiveMapPreemptEntry {
    long long seq = -1;
    std::weak_ptr<std::atomic<bool>> cancel_flag;
};

std::mutex g_interactive_map_preempt_mu;
std::unordered_map<std::string, InteractiveMapPreemptEntry> g_interactive_map_preempt;

std::shared_ptr<std::atomic<bool>> registerInteractiveMapRequest(const std::string& key, long long seq) {
    auto flag = std::make_shared<std::atomic<bool>>(false);
    if (key.empty()) return flag;

    std::lock_guard<std::mutex> lk(g_interactive_map_preempt_mu);
    auto& entry = g_interactive_map_preempt[key];
    if (seq >= entry.seq) {
        if (auto previous = entry.cancel_flag.lock()) {
            previous->store(true, std::memory_order_relaxed);
        }
        entry.seq = seq;
        entry.cancel_flag = flag;
    } else {
        flag->store(true, std::memory_order_relaxed);
    }
    return flag;
}

bool isCancelledException(const std::exception& ex) {
    return std::string(ex.what()) == "cancelled";
}

// Resolve a variant string into (Variant enum, optional custom step fn).
// Custom variants use the "custom:HASH" prefix and look up the dlopen registry.
struct VariantResolved {
    compute::Variant      var;
    compute::CustomStepFn fn = nullptr;  // non-null only for Variant::Custom
    double default_bailout = std::numeric_limits<double>::quiet_NaN();
    double default_bailout_sq = std::numeric_limits<double>::quiet_NaN();
};

VariantResolved resolveVariant(const std::string& s, const std::filesystem::path& repoRoot) {
    if (s.rfind("custom:", 0) == 0) {
        const std::string hash = s.substr(7);
        void* raw = lookupCustomFn(repoRoot, hash);
        if (!raw) throw std::runtime_error("custom variant not found or compile failed: " + hash);
        compute::CustomStepFn fn;
        std::memcpy(&fn, &raw, sizeof(fn));
        return {compute::Variant::Custom, fn,
                lookupCustomBailout(repoRoot, hash),
                lookupCustomBailoutSq(repoRoot, hash)};
    }
    compute::Variant v;
    if (compute::variant_from_name(s.c_str(), v)) {
        return {v, nullptr, compute::variant_default_bailout(v), compute::variant_default_bailout_sq(v)};
    }
    // Backwards compat with old integer IDs.
    try {
        int i = std::stoi(s);
        if (i >= 0 && i <= 15) {
            const auto v = static_cast<compute::Variant>(i);
            return {v, nullptr, compute::variant_default_bailout(v), compute::variant_default_bailout_sq(v)};
        }
    } catch (...) {}
    return {compute::Variant::Mandelbrot, nullptr,
            compute::variant_default_bailout(compute::Variant::Mandelbrot),
            compute::variant_default_bailout_sq(compute::Variant::Mandelbrot)};
}

double resolvedDefaultBailout(const VariantResolved& vr) {
    if (std::isfinite(vr.default_bailout) && vr.default_bailout > 0.0) return vr.default_bailout;
    return compute::variant_default_bailout(vr.var);
}

double resolvedDefaultBailoutSq(const VariantResolved& vr) {
    if (std::isfinite(vr.default_bailout_sq) && vr.default_bailout_sq > 0.0) return vr.default_bailout_sq;
    const double radius = resolvedDefaultBailout(vr);
    return radius * radius;
}

double bailoutSqFromJson(const Json& j, double radius, double defaultSq) {
    if (j.contains("bailoutSq") && !j["bailoutSq"].is_null()) {
        return j.value("bailoutSq", defaultSq);
    }
    if (j.contains("bailout") && !j["bailout"].is_null()) {
        return radius * radius;
    }
    return defaultSq;
}

compute::Metric parseMetric(const std::string& s) {
    compute::Metric m;
    if (compute::metric_from_name(s.c_str(), m)) return m;
    return compute::Metric::Escape;
}

compute::Colormap parseColormap(const std::string& s) {
    compute::Colormap c;
    if (compute::colormap_from_name(s.c_str(), c)) return c;
    return compute::Colormap::ClassicCos;
}

compute::Variant parseBuiltinVariant(const std::string& s, compute::Variant fallback) {
    compute::Variant v;
    if (compute::variant_from_name(s.c_str(), v)) return v;
    return fallback;
}

double normalizeTransitionTheta(double theta) {
    if (!std::isfinite(theta)) throw std::runtime_error("invalid transitionTheta");
    if (std::abs(theta) > TAU + 1e-6) {
        theta = theta * PI / 180.0;
    }
    return std::remainder(theta, TAU);
}

int normalizeTransitionMilliDeg(long long milliDeg) {
    long long wrapped = (milliDeg + THETA_HALF_TURN_MDEG) % THETA_FULL_TURN_MDEG;
    if (wrapped < 0) wrapped += THETA_FULL_TURN_MDEG;
    wrapped -= THETA_HALF_TURN_MDEG;
    if (wrapped == -THETA_HALF_TURN_MDEG && milliDeg > 0) {
        wrapped = THETA_HALF_TURN_MDEG;
    }
    return static_cast<int>(wrapped);
}

int thetaToMilliDeg(double theta) {
    const double radians = normalizeTransitionTheta(theta);
    return normalizeTransitionMilliDeg(
        static_cast<long long>(std::llround(radians * 180.0 * THETA_SCALE / PI)));
}

struct MapRenderInput {
    Json j;
    double cRe = -0.75;
    double cIm = 0.0;
    double scale = 3.0;
    int width = 1024;
    int height = 768;
    int iters = 1024;
    std::string variantStr = "mandelbrot";
    std::string metricStr = "escape";
    std::string colormapStr = "classic_cos";
    bool julia = false;
    double juliaRe = 0.0;
    double juliaIm = 0.0;
    std::string requestId;
    std::string preemptKey;
    long long preemptSeq = 0;
    bool hasTheta = false;
    int thetaMilliDeg = 0;
    double theta = 0.0;
    std::string scalarType = "auto";
    std::string engine = "openmp";
    bool smooth = false;
    bool stillExport = false;
    bool localExport = false;
    std::string colorMode = "direct";   // direct | eq_full | eq_center (equalized preview)
    double cyclesPerOctave = 1.0;        // band density for equalized modes
};

struct MapRenderImage {
    cv::Mat image;
    std::string artifactName = "map.png";
    std::string scalarUsed = "fp64";
    std::string engineUsed = "openmp";
    double elapsed = 0.0;
    double effectiveBailout = 2.0;
    double effectiveBailoutSq = 4.0;
};

MapRenderInput parseMapRenderInput(const std::string& body) {
    MapRenderInput in;
    in.j = parseJsonBody(body);
    const Json& j = in.j;

    in.cRe = j.value("centerRe", -0.75);
    in.cIm = j.value("centerIm", 0.0);
    in.scale = j.value("scale", 3.0);
    in.width = j.value("width", 1024);
    in.height = j.value("height", 768);
    in.iters = j.value("iterations", 1024);
    in.variantStr = j.value("variant", std::string("mandelbrot"));
    in.metricStr = j.value("metric", std::string("escape"));
    in.colormapStr = j.value("colorMap", std::string("classic_cos"));
    in.julia = j.value("julia", false);
    in.juliaRe = j.value("juliaRe", 0.0);
    in.juliaIm = j.value("juliaIm", 0.0);
    in.requestId = j.value("requestId", std::string(""));
    in.preemptKey = j.value("preemptKey", std::string(""));
    in.preemptSeq = j.value("preemptSeq", 0LL);

    const bool hasThetaMilliDeg =
        j.contains("transitionThetaMilliDeg") && !j["transitionThetaMilliDeg"].is_null();
    in.hasTheta = hasThetaMilliDeg || (j.contains("transitionTheta") && !j["transitionTheta"].is_null());
    in.thetaMilliDeg = hasThetaMilliDeg
        ? normalizeTransitionMilliDeg(j.value("transitionThetaMilliDeg", 0LL))
        : (in.hasTheta ? thetaToMilliDeg(j.value("transitionTheta", 0.0)) : 0);
    in.theta = static_cast<double>(in.thetaMilliDeg) * PI / (180.0 * THETA_SCALE);

    in.scalarType = j.value("scalarType", std::string("auto"));
    in.engine = j.value("engine", std::string("openmp"));
    in.smooth = j.value("smooth", false);
    in.stillExport = j.value("taskType", std::string("")) == "still_export";
    in.localExport = j.value("localExport", false);
    in.colorMode = j.value("colorMode", std::string("direct"));
    in.cyclesPerOctave = j.value("cyclesPerOctave", 1.0);

    if (!(in.scale > 0.0) || !std::isfinite(in.scale)) throw std::runtime_error("invalid scale");
    if (in.width < 64 || in.width > MAX_MAP_DIM) throw std::runtime_error("invalid width");
    if (in.height < 64 || in.height > MAX_MAP_DIM) throw std::runtime_error("invalid height");
    if (in.iters < 1 || in.iters > 1000000) throw std::runtime_error("invalid iterations");
    if (!std::isfinite(in.cRe) || !std::isfinite(in.cIm)) throw std::runtime_error("invalid center");
    if (in.colorMode != "direct" && in.colorMode != "eq_full" && in.colorMode != "eq_center")
        throw std::runtime_error("invalid colorMode (direct|eq_full|eq_center)");
    if (!(in.cyclesPerOctave > 0.0) || in.cyclesPerOctave > 64.0 || !std::isfinite(in.cyclesPerOctave))
        throw std::runtime_error("invalid cyclesPerOctave (0..64)");

    return in;
}

std::shared_ptr<std::atomic<bool>> registerMapRenderPreempt(const MapRenderInput& in) {
    const bool interactivePreempt = !in.stillExport && !in.preemptKey.empty() && !in.requestId.empty();
    return interactivePreempt
        ? registerInteractiveMapRequest(in.preemptKey, in.preemptSeq)
        : std::shared_ptr<std::atomic<bool>>{};
}

Json cancelledMapRenderJson(const std::string& runId, const MapRenderInput& in) {
    Json resp = {
        {"status", "cancelled"},
        {"requestId", in.requestId},
    };
    if (!runId.empty()) resp["runId"] = runId;
    return resp;
}

Json mapRenderEffectiveJson(const MapRenderInput& in, const MapRenderImage& rendered) {
    return {
        {"centerRe", in.cRe},
        {"centerIm", in.cIm},
        {"scale", in.scale},
        {"iterations", in.iters},
        {"variant", in.variantStr},
        {"metric", in.metricStr},
        {"colorMap", in.colormapStr},
        {"bailout", rendered.effectiveBailout},
        {"bailoutSq", rendered.effectiveBailoutSq},
        {"julia", in.julia},
        {"juliaRe", in.juliaRe},
        {"juliaIm", in.juliaIm},
        {"transitionTheta", in.hasTheta ? in.theta : 0.0},
        {"transitionThetaMilliDeg", in.hasTheta ? in.thetaMilliDeg : 0},
        {"transitionActive", in.hasTheta},
        {"transitionFrom", in.hasTheta ? in.j.value("transitionFrom", std::string("mandelbrot")) : std::string("")},
        {"transitionTo", in.hasTheta ? in.j.value("transitionTo", std::string("burning_ship")) : std::string("")},
    };
}

void throwIfMapRenderCancelled(const std::function<bool()>& shouldCancel) {
    if (shouldCancel && shouldCancel()) throw std::runtime_error("cancelled");
}

MapRenderImage renderMapImage(const std::filesystem::path& repoRoot,
                              const MapRenderInput& in,
                              const std::function<bool()>& shouldCancel) {
    MapRenderImage rendered;
    const Json& j = in.j;

    if (in.hasTheta) {
        throwIfMapRenderCancelled(shouldCancel);
        double bailout = j.contains("bailout") && !j["bailout"].is_null()
            ? j.value("bailout", 2.0)
            : 2.0;
        const double bailoutSq = bailoutSqFromJson(j, bailout, 4.0);
        if (j.contains("bailoutSq") && !j["bailoutSq"].is_null() &&
            !(j.contains("bailout") && !j["bailout"].is_null())) {
            bailout = std::sqrt(bailoutSq);
        }
        if (!(bailout > 0.0) || !std::isfinite(bailout)) throw std::runtime_error("invalid bailout");
        if (!(bailoutSq > 0.0) || !std::isfinite(bailoutSq)) throw std::runtime_error("invalid bailoutSq");
        rendered.effectiveBailout = bailout;
        rendered.effectiveBailoutSq = bailoutSq;
        const compute::Variant fromVariant = parseBuiltinVariant(
            j.value("transitionFrom", std::string("mandelbrot")),
            compute::Variant::Mandelbrot);
        const compute::Variant toVariant = parseBuiltinVariant(
            j.value("transitionTo", std::string("burning_ship")),
            compute::Variant::Boat);
        if (!compute::variant_supports_axis_transition(fromVariant) ||
            !compute::variant_supports_axis_transition(toVariant)) {
            throw std::runtime_error("transition variants must be quadratic Mandelbrot-family variants");
        }
        compute::TransitionParams p;
        p.center_re = in.cRe;
        p.center_im = in.cIm;
        p.scale = in.scale;
        p.width = in.width;
        p.height = in.height;
        p.iterations = in.iters;
        p.bailout = bailout;
        p.bailout_sq = bailoutSq;
        p.theta = in.theta;
        p.theta_milli_deg_set = true;
        p.theta_milli_deg = in.thetaMilliDeg;
        p.metric = parseMetric(in.metricStr);
        p.colormap = parseColormap(in.colormapStr);
        p.smooth = in.smooth;
        p.pairwise_cap = j.value("pairwiseCap", 64);
        if (p.pairwise_cap < 1 || p.pairwise_cap > 1000000) throw std::runtime_error("invalid pairwiseCap");
        p.from_variant = fromVariant;
        p.to_variant = toVariant;
        p.scalar_type = in.scalarType;
        p.engine = in.engine;
        p.should_cancel = shouldCancel;
        auto stats = compute::render_transition(p, rendered.image);
        throwIfMapRenderCancelled(shouldCancel);
        rendered.elapsed = stats.elapsed_ms;
        rendered.scalarUsed = stats.scalar_used;
        rendered.engineUsed = stats.engine_used;
        rendered.artifactName = "transition.png";
        return rendered;
    }

    throwIfMapRenderCancelled(shouldCancel);
    const auto vr = resolveVariant(in.variantStr, repoRoot);
    double bailout = j.contains("bailout") && !j["bailout"].is_null()
        ? j.value("bailout", 2.0)
        : resolvedDefaultBailout(vr);
    const double bailoutSq = bailoutSqFromJson(j, bailout, resolvedDefaultBailoutSq(vr));
    if (j.contains("bailoutSq") && !j["bailoutSq"].is_null() &&
        !(j.contains("bailout") && !j["bailout"].is_null())) {
        bailout = std::sqrt(bailoutSq);
    }
    if (!(bailout > 0.0) || !std::isfinite(bailout)) throw std::runtime_error("invalid bailout");
    if (!(bailoutSq > 0.0) || !std::isfinite(bailoutSq)) throw std::runtime_error("invalid bailoutSq");
    rendered.effectiveBailout = bailout;
    rendered.effectiveBailoutSq = bailoutSq;

    compute::MapParams p;
    p.center_re = in.cRe;
    p.center_im = in.cIm;
    p.scale = in.scale;
    if (j.contains("centerReStr") && j["centerReStr"].is_string())
        p.center_re_str = j["centerReStr"].get<std::string>();
    if (j.contains("centerImStr") && j["centerImStr"].is_string())
        p.center_im_str = j["centerImStr"].get<std::string>();
    p.width = in.width;
    p.height = in.height;
    p.iterations = in.iters;
    p.bailout = bailout;
    p.bailout_sq = bailoutSq;
    p.variant = vr.var;
    p.custom_step_fn = vr.fn;
    p.metric = parseMetric(in.metricStr);
    p.colormap = parseColormap(in.colormapStr);
    p.pairwise_cap = j.value("pairwiseCap", 64);
    if (p.pairwise_cap < 1 || p.pairwise_cap > 1000000) throw std::runtime_error("invalid pairwiseCap");
    p.julia = in.julia;
    p.julia_re = in.juliaRe;
    p.julia_im = in.juliaIm;
    p.scalar_type = in.scalarType;
    p.engine = in.engine;
    p.smooth = in.smooth;
    p.should_cancel = shouldCancel;

    // Equalized preview modes: render the raw escape field, build a periodic equalization
    // from it (full-image or center-weighted, faithful to the ln-map), then colorize. Only
    // the escape metric carries per-pixel iteration counts; anything else falls back to the
    // normal inline-colorized render. render_map_field is OpenMP-only (no CUDA/AVX), which is
    // acceptable for an opt-in preview and is reported honestly via stats.engine_used.
    if (in.colorMode != "direct" && p.metric == compute::Metric::Escape) {
        compute::FieldOutput fo;
        auto stats = compute::render_map_field(p, fo);
        throwIfMapRenderCancelled(shouldCancel);
        const auto eq = compute::build_map_equalization(p, fo, in.colorMode == "eq_center", in.cyclesPerOctave);
        compute::colorize_map_field_equalized(p, fo, eq, rendered.image);
        throwIfMapRenderCancelled(shouldCancel);
        rendered.elapsed = stats.elapsed_ms;
        rendered.scalarUsed = stats.scalar_used;
        rendered.engineUsed = stats.engine_used;
        rendered.artifactName = "map.png";
        return rendered;
    }

    auto stats = compute::render_map(p, rendered.image);
    throwIfMapRenderCancelled(shouldCancel);
    rendered.elapsed = stats.elapsed_ms;
    rendered.scalarUsed = stats.scalar_used;
    rendered.engineUsed = stats.engine_used;
    rendered.artifactName = "map.png";
    return rendered;
}

std::string safeHeaderValue(std::string value) {
    for (char& ch : value) {
        if (ch == '\r' || ch == '\n') ch = ' ';
    }
    return value;
}

std::string inlineMapHeaders(const MapRenderInput& in, const std::string& status) {
    return std::string("Cache-Control: no-store\r\n") +
        "X-FSD-Status: " + safeHeaderValue(status) + "\r\n" +
        "X-FSD-Request-Id: " + safeHeaderValue(in.requestId) + "\r\n";
}

std::string inlineMapHeaders(const MapRenderInput& in, const MapRenderImage& rendered) {
    return inlineMapHeaders(in, "completed") +
        "X-FSD-Generated-Ms: " + std::to_string(rendered.elapsed) + "\r\n" +
        "X-FSD-Engine: " + safeHeaderValue(rendered.engineUsed) + "\r\n" +
        "X-FSD-Scalar: " + safeHeaderValue(rendered.scalarUsed) + "\r\n" +
        "X-FSD-Width: " + std::to_string(in.width) + "\r\n" +
        "X-FSD-Height: " + std::to_string(in.height) + "\r\n" +
        "X-FSD-Pixel-Format: rgba8\r\n";
}

} // namespace

std::string mapRenderRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& body) {
    const MapRenderInput in = parseMapRenderInput(body);
    auto preemptFlag = registerMapRenderPreempt(in);
    std::function<bool()> shouldCancel = [preemptFlag]() {
        return preemptFlag && preemptFlag->load(std::memory_order_relaxed);
    };

    auto run = runner.createRun(in.stillExport ? "map-export" : "map", body);
    if (shouldCancel()) {
        runner.setStatus(run.id, "cancelled");
        return cancelledMapRenderJson(run.id, in).dump();
    }

    ResourceManager::Lease heavyLease;
    if (in.stillExport) {
        std::string conflictLock, activeRunId;
        if (!resourceManager().tryAcquire(run.id, "still_export", {"cuda_heavy", "cpu_heavy"}, heavyLease, conflictLock, activeRunId)) {
            runner.setStatus(run.id, "failed");
            throw HttpError(409, Json{
                {"error", "heavy render already running"},
                {"activeRunId", activeRunId},
                {"taskType", "still_export"},
                {"resourceLock", conflictLock},
            }.dump());
        }
        runner.setCancelable(run.id, false);
        runner.setProgress(run.id, Json{
            {"taskType", "map_export"},
            {"stage", "render"},
            {"current", 0},
            {"total", 1},
            {"percent", 0.0},
            {"engine", in.engine},
            {"scalar", in.scalarType},
            {"elapsedMs", runner.runElapsedMs(run.id)},
            {"cancelable", false},
            {"resourceLocks", Json::array({"cuda_heavy", "cpu_heavy"})},
            {"details", Json::object()},
        }.dump());
    }
    (void)heavyLease;
    runner.setStatus(run.id, "running");

    MapRenderImage rendered;
    std::filesystem::path imagePath;
    try {
        rendered = renderMapImage(repoRoot, in, shouldCancel);
        throwIfMapRenderCancelled(shouldCancel);
        imagePath = std::filesystem::path(run.outputDir) / rendered.artifactName;
        compute::write_png(imagePath.string(), rendered.image);
        runner.addArtifact(run.id, Artifact{"map", imagePath.string(), "image"});
        if (in.stillExport) {
            runner.setProgress(run.id, Json{
                {"taskType", "map_export"},
                {"stage", "completed"},
                {"current", 1},
                {"total", 1},
                {"percent", 100.0},
                {"engine", rendered.engineUsed},
                {"scalar", rendered.scalarUsed},
                {"elapsedMs", runner.runElapsedMs(run.id)},
                {"cancelable", false},
                {"resourceLocks", Json::array({"cuda_heavy", "cpu_heavy"})},
                {"details", Json::object()},
            }.dump());
        }
        runner.setStatus(run.id, "completed");
    } catch (const std::exception& ex) {
        if (isCancelledException(ex)) {
            runner.setStatus(run.id, "cancelled");
            return cancelledMapRenderJson(run.id, in).dump();
        }
        runner.setStatus(run.id, "failed");
        throw;
    }

    const std::string artifactId = run.id + ":" + rendered.artifactName;
    Json resp = {
        {"runId", run.id},
        {"status", "completed"},
        {"artifactId", artifactId},
        {"imagePath", "/api/artifacts/content?artifactId=" + artifactId},
        {"localPath", imagePath.string()},
        {"localExport", in.localExport},
        {"generatedMs", rendered.elapsed},
        {"width", in.width},
        {"height", in.height},
        {"scalarUsed", rendered.scalarUsed},
        {"engineUsed", rendered.engineUsed},
        {"requestId", in.requestId},
        {"effective", mapRenderEffectiveJson(in, rendered)},
    };
    return resp.dump();
}

std::string mapRenderInlineRoute(const std::filesystem::path& repoRoot,
                                 const std::string& body,
                                 int& status,
                                 std::string& contentType,
                                 std::string& extraHeaders) {
    const MapRenderInput in = parseMapRenderInput(body);
    if (in.stillExport) {
        throw HttpError(400, Json{{"error", "render-inline does not create artifacts"}}.dump());
    }

    contentType = "application/octet-stream";
    auto preemptFlag = registerMapRenderPreempt(in);
    std::function<bool()> shouldCancel = [preemptFlag]() {
        return preemptFlag && preemptFlag->load(std::memory_order_relaxed);
    };
    if (shouldCancel()) {
        status = 204;
        extraHeaders = inlineMapHeaders(in, "cancelled");
        return {};
    }

    try {
        const MapRenderImage rendered = renderMapImage(repoRoot, in, shouldCancel);
        throwIfMapRenderCancelled(shouldCancel);
        std::string pixels = compute::encode_rgba8(rendered.image);
        status = 200;
        extraHeaders = inlineMapHeaders(in, rendered);
        return pixels;
    } catch (const std::exception& ex) {
        if (isCancelledException(ex)) {
            status = 204;
            extraHeaders = inlineMapHeaders(in, "cancelled");
            return {};
        }
        throw;
    }
}

std::string mapPreemptRoute(const std::string& body) {
    const Json j = parseJsonBody(body);
    const std::string preemptKey = j.value("preemptKey", std::string(""));
    const long long preemptSeq = j.value("preemptSeq", 0LL);
    if (!preemptKey.empty()) {
        registerInteractiveMapRequest(preemptKey, preemptSeq);
    }
    return Json{
        {"status", "ok"},
        {"preemptKey", preemptKey},
        {"preemptSeq", preemptSeq},
    }.dump();
}

// ─── /api/map/field — raw field data (no colorization) ───────────────────────
//
// Returns base64-encoded raw metric values so the browser can colorize
// instantly on colormap change without re-fetching.
//
// Escape metric:    uint32[W*H] iter counts  (iterB64)
//                   float32[W*H] |z|² at escape, 0 if bounded  (finalMagB64)
// Non-escape metric: float64[W*H] raw values  (fieldB64) + fieldMin, fieldMax
//
// This endpoint is intentionally run-store-free (high-frequency tile calls):
// no artifacts are written and no run row is created.

std::string mapFieldRoute(const std::filesystem::path& repoRoot, const std::string& body) {
    const Json j = parseJsonBody(body);

    const double cRe     = j.value("centerRe",  -0.75);
    const double cIm     = j.value("centerIm",   0.0);
    const double scale   = j.value("scale",      3.0);
    const int width      = j.value("width",      256);
    const int height     = j.value("height",     256);
    const int iters      = j.value("iterations", 1024);
    const std::string variantStr  = j.value("variant",    std::string("mandelbrot"));
    const std::string metricStr   = j.value("metric",     std::string("escape"));
    const bool julia              = j.value("julia",      false);
    const double juliaRe          = j.value("juliaRe",    0.0);
    const double juliaIm          = j.value("juliaIm",    0.0);
    const std::string scalarType  = j.value("scalarType", std::string("auto"));
    const std::string engine      = j.value("engine",     std::string("auto"));

    if (!(scale > 0.0) || !std::isfinite(scale))   throw std::runtime_error("invalid scale");
    if (width  < 1 || width  > MAX_MAP_DIM)               throw std::runtime_error("invalid width");
    if (height < 1 || height > MAX_MAP_DIM)               throw std::runtime_error("invalid height");
    if (iters  < 1 || iters  > 1000000)            throw std::runtime_error("invalid iterations");
    if (!std::isfinite(cRe) || !std::isfinite(cIm)) throw std::runtime_error("invalid center");

    const auto vr2 = resolveVariant(variantStr, repoRoot);
    double bailout = j.contains("bailout") && !j["bailout"].is_null()
        ? j.value("bailout", 2.0)
        : resolvedDefaultBailout(vr2);
    const double bailoutSq = bailoutSqFromJson(j, bailout, resolvedDefaultBailoutSq(vr2));
    if (j.contains("bailoutSq") && !j["bailoutSq"].is_null() &&
        !(j.contains("bailout") && !j["bailout"].is_null())) {
        bailout = std::sqrt(bailoutSq);
    }
    if (!(bailout > 0.0) || !std::isfinite(bailout)) throw std::runtime_error("invalid bailout");
    if (!(bailoutSq > 0.0) || !std::isfinite(bailoutSq)) throw std::runtime_error("invalid bailoutSq");
    compute::MapParams p;
    p.center_re  = cRe;
    p.center_im  = cIm;
    p.scale      = scale;
    if (j.contains("centerReStr") && j["centerReStr"].is_string())
        p.center_re_str = j["centerReStr"].get<std::string>();
    if (j.contains("centerImStr") && j["centerImStr"].is_string())
        p.center_im_str = j["centerImStr"].get<std::string>();
    p.width      = width;
    p.height     = height;
    p.iterations = iters;
    p.bailout    = bailout;
    p.bailout_sq = bailoutSq;
    p.variant    = vr2.var;
    p.custom_step_fn = vr2.fn;
    p.metric     = parseMetric(metricStr);
    p.pairwise_cap = j.value("pairwiseCap", 64);
    if (p.pairwise_cap < 1 || p.pairwise_cap > 1000000) throw std::runtime_error("invalid pairwiseCap");
    p.julia      = julia;
    p.julia_re   = juliaRe;
    p.julia_im   = juliaIm;
    p.scalar_type = scalarType;
    p.engine      = engine;

    compute::FieldOutput fo;
    const auto stats = compute::render_map_field(p, fo);

    Json resp = {
        {"status",      "completed"},
        {"width",       width},
        {"height",      height},
        {"metric",      metricStr},
        {"generatedMs", stats.elapsed_ms},
        {"scalarUsed",  stats.scalar_used},
        {"maxIter",     iters},
    };

    if (fo.metric == compute::Metric::Escape) {
        resp["iterB64"]     = base64Encode(
            reinterpret_cast<const uint8_t*>(fo.iter_u32.data()),
            fo.iter_u32.size() * sizeof(uint32_t));
        resp["finalMagB64"] = base64Encode(
            reinterpret_cast<const uint8_t*>(fo.norm_f32.data()),
            fo.norm_f32.size() * sizeof(float));
    } else {
        resp["fieldB64"]  = base64Encode(
            reinterpret_cast<const uint8_t*>(fo.field_f64.data()),
            fo.field_f64.size() * sizeof(double));
        resp["fieldMin"]  = fo.field_min;
        resp["fieldMax"]  = fo.field_max;
    }

    return resp.dump();
}

} // namespace fsd
