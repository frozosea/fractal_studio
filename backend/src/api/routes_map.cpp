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
#include "../compute/colorize.hpp"
#include "../compute/ln_map.hpp"
#include "../compute/tile_scheduler.hpp"
#include "../compute/transition_kernel.hpp"
#include "../compute/image_io.hpp"
#include "../compute/variants.hpp"
#include "../compute/escape_time.hpp"
#include "../compute/colormap.hpp"

#include <cmath>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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
constexpr size_t MAX_INTERACTIVE_MAP_PREEMPT_ENTRIES = 256;
constexpr size_t MAX_INTERACTIVE_MAP_PREEMPT_KEY_BYTES = 128;

void pruneInteractiveMapPreemptEntriesLocked() {
    for (auto it = g_interactive_map_preempt.begin(); it != g_interactive_map_preempt.end();) {
        if (it->second.cancel_flag.expired()) it = g_interactive_map_preempt.erase(it);
        else ++it;
    }
}

std::shared_ptr<std::atomic<bool>> registerInteractiveMapRequest(const std::string& key, long long seq) {
    auto flag = std::make_shared<std::atomic<bool>>(false);
    if (key.empty()) return flag;
    if (key.size() > MAX_INTERACTIVE_MAP_PREEMPT_KEY_BYTES || seq < 0) {
        flag->store(true, std::memory_order_relaxed);
        return flag;
    }

    std::lock_guard<std::mutex> lk(g_interactive_map_preempt_mu);
    if (g_interactive_map_preempt.size() >= MAX_INTERACTIVE_MAP_PREEMPT_ENTRIES) {
        pruneInteractiveMapPreemptEntriesLocked();
    }
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
    std::shared_ptr<void> custom_lease;
};

VariantResolved resolveVariant(const std::string& s, const std::filesystem::path& repoRoot) {
    if (s.rfind("custom:", 0) == 0) {
        const std::string hash = s.substr(7);
        const CustomVariantLease lease = acquireCustomVariantLease(repoRoot, hash);
        void* raw = lease.function;
        if (!raw) throw std::runtime_error("custom variant not found or compile failed: " + hash);
        compute::CustomStepFn fn;
        std::memcpy(&fn, &raw, sizeof(fn));
        return {compute::Variant::Custom, fn,
                lease.bailout, lease.bailoutSq, lease.library};
    }
    compute::Variant v;
    if (compute::variant_from_name(s.c_str(), v)) {
        return {v, nullptr, compute::variant_default_bailout(v), compute::variant_default_bailout_sq(v), {}};
    }
    // Backwards compat with old integer IDs.
    try {
        int i = std::stoi(s);
        if (i >= 0 && i <= 15) {
            const auto v = static_cast<compute::Variant>(i);
            return {v, nullptr, compute::variant_default_bailout(v), compute::variant_default_bailout_sq(v), {}};
        }
    } catch (...) {}
    return {compute::Variant::Mandelbrot, nullptr,
            compute::variant_default_bailout(compute::Variant::Mandelbrot),
            compute::variant_default_bailout_sq(compute::Variant::Mandelbrot), {}};
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

std::string variantJsonToString(const Json& value, const std::string& fallback = "mandelbrot") {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(value.get<int>());
    return fallback;
}

std::vector<compute::TransitionLeg> parseTransitionLegs(const Json& j) {
    std::vector<compute::TransitionLeg> legs;
    if (j.contains("transitionLegs") && j["transitionLegs"].is_array()) {
        for (const Json& item : j["transitionLegs"]) {
            std::string variant = "mandelbrot";
            double weight = 1.0;
            if (item.is_object()) {
                variant = item.contains("variant")
                    ? variantJsonToString(item["variant"])
                    : std::string("mandelbrot");
                weight = item.value("weight", 1.0);
            } else {
                variant = variantJsonToString(item);
            }
            legs.push_back({parseBuiltinVariant(variant, compute::Variant::Mandelbrot), weight});
        }
        return legs;
    }

    if (j.contains("transitionVariants") && j["transitionVariants"].is_array()) {
        const Json& variants = j["transitionVariants"];
        const Json* weights = (j.contains("transitionWeights") && j["transitionWeights"].is_array())
            ? &j["transitionWeights"]
            : nullptr;
        for (size_t i = 0; i < variants.size(); ++i) {
            const std::string variant = variantJsonToString(variants[i]);
            const double weight = (weights && i < weights->size() && (*weights)[i].is_number())
                ? (*weights)[i].get<double>()
                : 1.0;
            legs.push_back({parseBuiltinVariant(variant, compute::Variant::Mandelbrot), weight});
        }
    }
    return legs;
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
    double viewportAspect = 0.0;
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
    double rotationDeg = 0.0;
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

    in.cRe = resolveCenterCoord(
        (j.contains("centerReStr") && j["centerReStr"].is_string()) ? j["centerReStr"].get<std::string>() : std::string(),
        j.value("centerRe", -0.75));
    in.cIm = resolveCenterCoord(
        (j.contains("centerImStr") && j["centerImStr"].is_string()) ? j["centerImStr"].get<std::string>() : std::string(),
        j.value("centerIm", 0.0));
    in.scale = j.value("scale", 3.0);
    in.viewportAspect = j.value("viewportAspect", 0.0);
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
    in.rotationDeg = j.value("rotationDeg", 0.0);
    if (!std::isfinite(in.rotationDeg)) in.rotationDeg = 0.0;

    if (!(in.scale > 0.0) || !std::isfinite(in.scale)) throw std::runtime_error("invalid scale");
    if (j.contains("viewportAspect") &&
        (!(in.viewportAspect > 0.0) || !std::isfinite(in.viewportAspect))) {
        throw std::runtime_error("invalid viewportAspect");
    }
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
    Json resp = {
        {"centerRe", in.cRe},
        {"centerIm", in.cIm},
        {"scale", in.scale},
        {"viewportAspect", in.viewportAspect > 0.0
            ? in.viewportAspect
            : static_cast<double>(in.width) / static_cast<double>(in.height)},
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
        {"rotationDeg", in.rotationDeg},
    };
    if (in.hasTheta && in.j.contains("transitionVariants")) {
        resp["transitionVariants"] = in.j["transitionVariants"];
    }
    if (in.hasTheta && in.j.contains("transitionWeights")) {
        resp["transitionWeights"] = in.j["transitionWeights"];
    }
    if (in.hasTheta && in.j.contains("transitionLegs")) {
        resp["transitionLegs"] = in.j["transitionLegs"];
    }
    return resp;
}

void throwIfMapRenderCancelled(const std::function<bool()>& shouldCancel) {
    if (shouldCancel && shouldCancel()) throw std::runtime_error("cancelled");
}

compute::MapParams buildMapParams(const MapRenderInput& in, const Json& j,
                                  double bailout, double bailoutSq,
                                  const std::function<bool()>& shouldCancel) {
    compute::MapParams p;
    p.center_re = in.cRe;
    p.center_im = in.cIm;
    if (j.contains("centerReStr") && j["centerReStr"].is_string())
        p.center_re_str = j["centerReStr"].get<std::string>();
    if (j.contains("centerImStr") && j["centerImStr"].is_string())
        p.center_im_str = j["centerImStr"].get<std::string>();
    p.scale = in.scale;
    p.viewport_aspect = in.viewportAspect;
    p.width = in.width;
    p.height = in.height;
    p.iterations = in.iters;
    p.bailout = bailout;
    p.bailout_sq = bailoutSq;
    p.metric = parseMetric(in.metricStr);
    p.colormap = parseColormap(in.colormapStr);
    p.smooth = in.smooth;
    p.pairwise_cap = j.value("pairwiseCap", 64);
    if (p.pairwise_cap < 1 || p.pairwise_cap > 1000000)
        throw std::runtime_error("invalid pairwiseCap");
    p.scalar_type = in.scalarType;
    p.engine = in.engine;
    p.rotation_deg = in.rotationDeg;
    p.should_cancel = shouldCancel;
    return p;
}

// ─── Interactive native-field sessions ──────────────────────────────────────
//
// A slow explorer render is still one native-resolution field computation.
// The session owns its FieldOutput until completion and publishes only tiles
// that have been completely written.  The UI can lower *presentation*
// resolution after its latency budget, but it never starts a second low-res
// fractal render or discards work solely because that budget elapsed.

enum class InteractiveFieldState {
    Running,
    Completed,
    Cancelled,
    Failed,
};

const char* interactiveFieldStateName(InteractiveFieldState state) {
    switch (state) {
        case InteractiveFieldState::Running:   return "running";
        case InteractiveFieldState::Completed: return "completed";
        case InteractiveFieldState::Cancelled: return "cancelled";
        case InteractiveFieldState::Failed:    return "failed";
    }
    return "failed";
}

struct InteractiveFieldSession {
    std::string id;
    std::string viewKey;
    std::string requestId;
    std::string requestFingerprint;
    std::string preemptKey;
    std::shared_ptr<std::atomic<bool>> cancelFlag;
    std::shared_ptr<void> customVariantLease;
    compute::MapParams params;
    compute::Colormap previewColorMap = compute::Colormap::ClassicCos;
    bool previewSmooth = false;
    int slowAfterMs = 450;
    uint64_t reservedBytes = 0;
    // A completed field stays pinned until the browser confirms it decoded
    // /result. The terminal-session cap must not turn a still-awaited final
    // field into a surprising 404.
    bool resultDelivered = false;
    // Base64 encoding reads the immutable raw vectors without holding `mu`.
    // Keep its reservation live until every such reader exits, even if an ack
    // races in, so a new session cannot over-admit memory in that gap.
    uint32_t resultReaders = 0;

    // `field` is allocated once by render_map_field. A callback publishes a
    // rectangle only after its writes have completed; readers hold `mu` while
    // checking/copying published samples.
    compute::FieldOutput field;
    std::vector<uint8_t> published;
    std::mutex mu;
    InteractiveFieldState state = InteractiveFieldState::Running;
    uint64_t revision = 0;
    uint64_t completedPixels = 0;
    compute::MapStats stats;
    std::string error;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point finished{};
    std::chrono::steady_clock::time_point lastAccess = started;
};

std::mutex g_interactive_field_sessions_mu;
std::unordered_map<std::string, std::shared_ptr<InteractiveFieldSession>> g_interactive_field_sessions;
std::unordered_map<std::string, std::string> g_interactive_field_by_view;
std::atomic<uint64_t> g_interactive_field_session_serial{0};
// Result responses temporarily allocate sizeable transport buffers. Serialize
// their construction across explorer sessions so a burst of tabs cannot turn
// one completed native field into several simultaneous base64 peaks.
std::atomic<bool> g_interactive_field_result_encoding{false};
constexpr auto INTERACTIVE_FIELD_SESSION_TTL = std::chrono::seconds(30);
constexpr auto INTERACTIVE_FIELD_IDLE_TTL = std::chrono::seconds(15);
constexpr auto INTERACTIVE_FIELD_REAPER_PERIOD = std::chrono::seconds(1);
constexpr size_t MAX_RETAINED_TERMINAL_INTERACTIVE_FIELDS = 3;
constexpr size_t MAX_RUNNING_INTERACTIVE_FIELDS = 2;
// Interactive sessions retain uint32 iterations, float norms, and a
// byte-sized publication map. Offline/raw-field APIs may support MAX_MAP_DIM,
// but a resumable explorer session is intentionally bounded to the browser's
// canvas size and a global reservation budget.
constexpr uint64_t INTERACTIVE_FIELD_BYTES_PER_PIXEL =
    sizeof(uint32_t) + sizeof(float) + sizeof(uint8_t);
constexpr uint64_t MAX_INTERACTIVE_FIELD_PIXELS = 4096ull * 4096ull;
constexpr uint64_t MAX_INTERACTIVE_FIELD_RESERVED_BYTES = 512ull * 1024ull * 1024ull;
constexpr int INTERACTIVE_FIELD_RETRY_AFTER_MS = 100;

bool interactiveFieldReservationBytes(int width, int height, uint64_t& bytes) {
    if (width <= 0 || height <= 0) return false;
    const uint64_t pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (pixels > MAX_INTERACTIVE_FIELD_PIXELS ||
        pixels > std::numeric_limits<uint64_t>::max() / INTERACTIVE_FIELD_BYTES_PER_PIXEL) {
        return false;
    }
    bytes = pixels * INTERACTIVE_FIELD_BYTES_PER_PIXEL;
    return bytes <= MAX_INTERACTIVE_FIELD_RESERVED_BYTES;
}

void eraseInteractiveFieldSessionLocked(const std::string& id) {
    const auto sessionIt = g_interactive_field_sessions.find(id);
    if (sessionIt == g_interactive_field_sessions.end()) return;
    const std::string viewKey = sessionIt->second->viewKey;
    g_interactive_field_sessions.erase(sessionIt);
    const auto byView = g_interactive_field_by_view.find(viewKey);
    if (byView != g_interactive_field_by_view.end() && byView->second == id) {
        g_interactive_field_by_view.erase(byView);
    }
}

// Sessions deliberately retain their native field long enough for the browser
// to promote the preview to its final result (or retry a dropped request), but
// they are not a general image cache. Completed fields which have not yet
// been acknowledged by the browser are deliberately exempt from the small
// terminal cap; that cap otherwise makes concurrent fast renders lose their
// final data.
void pruneInteractiveFieldSessionsLocked() {
    const auto now = std::chrono::steady_clock::now();
    struct TerminalSession {
        std::string id;
        std::chrono::steady_clock::time_point finished;
        bool disposable = false;
    };
    std::vector<TerminalSession> terminal;
    std::vector<std::string> remove;

    for (const auto& [id, session] : g_interactive_field_sessions) {
        std::lock_guard<std::mutex> sessionLock(session->mu);
        if (session->state == InteractiveFieldState::Running) {
            // Status/snapshot/result polling is the session heartbeat. A
            // disconnected browser must not leave a detached full-frame
            // worker alive forever.
            if (session->cancelFlag &&
                now - session->lastAccess > INTERACTIVE_FIELD_IDLE_TTL) {
                session->cancelFlag->store(true, std::memory_order_relaxed);
            }
            continue;
        }
        const bool hasResultReaders = session->resultReaders != 0;
        const bool expired = session->finished.time_since_epoch().count() != 0 &&
            now - session->finished > INTERACTIVE_FIELD_SESSION_TTL;
        if (expired && !hasResultReaders) {
            remove.push_back(id);
            continue;
        }
        terminal.push_back({
            id,
            session->finished,
            (session->state != InteractiveFieldState::Completed || session->resultDelivered) &&
                !hasResultReaders,
        });
    }

    std::vector<TerminalSession> disposable;
    for (const auto& item : terminal) {
        if (item.disposable) disposable.push_back(item);
    }
    std::sort(disposable.begin(), disposable.end(), [](const auto& a, const auto& b) {
        return a.finished < b.finished;
    });
    if (disposable.size() > MAX_RETAINED_TERMINAL_INTERACTIVE_FIELDS) {
        const size_t requiredRemovals = disposable.size() - MAX_RETAINED_TERMINAL_INTERACTIVE_FIELDS;
        for (size_t i = 0; i < requiredRemovals; ++i) {
            remove.push_back(disposable[i].id);
        }
    }

    for (const auto& id : remove) eraseInteractiveFieldSessionLocked(id);
}

size_t interactiveFieldRunningCountLocked() {
    size_t running = 0;
    for (const auto& [id, session] : g_interactive_field_sessions) {
        std::lock_guard<std::mutex> sessionLock(session->mu);
        // A cancel flag is only a request. A live OpenMP worker still consumes
        // a slot until it finalizes and releases its buffers.
        if (session->state == InteractiveFieldState::Running) ++running;
    }
    return running;
}

uint64_t interactiveFieldReservedBytesLocked() {
    uint64_t total = 0;
    for (const auto& [id, session] : g_interactive_field_sessions) {
        std::lock_guard<std::mutex> sessionLock(session->mu);
        if (session->reservedBytes > std::numeric_limits<uint64_t>::max() - total) {
            return std::numeric_limits<uint64_t>::max();
        }
        total += session->reservedBytes;
    }
    return total;
}

// Only results that the browser has acknowledged, and which have no active
// base64 reader, may be discarded early for admission. An unacknowledged completed field remains
// available for its TTL; callers receive a retryable overload response while
// that promise cannot be met within the memory budget.
void trimDeliveredInteractiveFieldsForBudgetLocked(uint64_t bytesNeeded) {
    uint64_t reserved = interactiveFieldReservedBytesLocked();
    if (bytesNeeded <= MAX_INTERACTIVE_FIELD_RESERVED_BYTES &&
        reserved <= MAX_INTERACTIVE_FIELD_RESERVED_BYTES - bytesNeeded) {
        return;
    }

    struct Candidate {
        std::string id;
        std::chrono::steady_clock::time_point finished;
        uint64_t reservedBytes;
    };
    std::vector<Candidate> candidates;
    for (const auto& [id, session] : g_interactive_field_sessions) {
        std::lock_guard<std::mutex> sessionLock(session->mu);
        if (session->state == InteractiveFieldState::Completed &&
            session->resultDelivered && session->resultReaders == 0 &&
            session->reservedBytes != 0) {
            candidates.push_back({id, session->finished, session->reservedBytes});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.finished < b.finished;
    });
    for (const auto& candidate : candidates) {
        if (bytesNeeded <= MAX_INTERACTIVE_FIELD_RESERVED_BYTES &&
            reserved <= MAX_INTERACTIVE_FIELD_RESERVED_BYTES - bytesNeeded) {
            break;
        }
        reserved = candidate.reservedBytes > reserved ? 0 : reserved - candidate.reservedBytes;
        eraseInteractiveFieldSessionLocked(candidate.id);
    }
}

// Interactive maps use all available OpenMP lanes per field. A cancellation
// request does not free a slot; it merely makes a live worker eligible to
// exit. When capacity is full, ask the oldest still-active view to stop and
// let the replacement retry only after a worker has finalized.
void makeRoomForInteractiveFieldLocked() {
    struct Candidate {
        std::shared_ptr<InteractiveFieldSession> session;
        std::chrono::steady_clock::time_point lastAccess;
    };
    std::vector<Candidate> running;
    size_t cancelledRunning = 0;
    for (const auto& [id, session] : g_interactive_field_sessions) {
        std::lock_guard<std::mutex> sessionLock(session->mu);
        const bool alreadyCancelled = session->cancelFlag &&
            session->cancelFlag->load(std::memory_order_relaxed);
        if (session->state == InteractiveFieldState::Running) {
            if (alreadyCancelled) {
                ++cancelledRunning;
            } else {
                running.push_back({session, session->lastAccess});
            }
        }
    }
    const size_t liveWorkers = running.size() + cancelledRunning;
    if (liveWorkers < MAX_RUNNING_INTERACTIVE_FIELDS) return;
    std::sort(running.begin(), running.end(), [](const Candidate& a, const Candidate& b) {
        return a.lastAccess < b.lastAccess;
    });
    // A single new worker is asking for admission. If one live worker is
    // already winding down, wait for it instead of evicting an unrelated
    // explorer just because the replacement retried.
    const size_t slotsToRelease = liveWorkers - MAX_RUNNING_INTERACTIVE_FIELDS + 1;
    const size_t cancelCount = slotsToRelease > cancelledRunning
        ? std::min(running.size(), slotsToRelease - cancelledRunning)
        : 0;
    for (size_t i = 0; i < cancelCount; ++i) {
        if (running[i].session->cancelFlag) {
            running[i].session->cancelFlag->store(true, std::memory_order_relaxed);
        }
    }
}

std::mutex g_interactive_field_reaper_mu;
std::condition_variable g_interactive_field_reaper_cv;
bool g_interactive_field_reaper_stopping = false;

class InteractiveFieldSessionReaper {
public:
    InteractiveFieldSessionReaper()
        : worker_([this]() { run(); }) {}

    ~InteractiveFieldSessionReaper() {
        {
            std::lock_guard<std::mutex> lk(g_interactive_field_reaper_mu);
            g_interactive_field_reaper_stopping = true;
        }
        g_interactive_field_reaper_cv.notify_all();
        if (worker_.joinable()) worker_.join();
    }

private:
    void run() {
        std::unique_lock<std::mutex> waitLock(g_interactive_field_reaper_mu);
        while (!g_interactive_field_reaper_stopping) {
            g_interactive_field_reaper_cv.wait_for(waitLock, INTERACTIVE_FIELD_REAPER_PERIOD);
            if (g_interactive_field_reaper_stopping) break;
            waitLock.unlock();
            {
                std::lock_guard<std::mutex> sessionsLock(g_interactive_field_sessions_mu);
                pruneInteractiveFieldSessionsLocked();
            }
            {
                std::lock_guard<std::mutex> preemptLock(g_interactive_map_preempt_mu);
                pruneInteractiveMapPreemptEntriesLocked();
            }
            waitLock.lock();
        }
    }

    std::thread worker_;
};

void ensureInteractiveFieldSessionReaper() {
    static InteractiveFieldSessionReaper reaper;
    (void)reaper;
}

void notifyInteractiveFieldSessionReaper() {
    ensureInteractiveFieldSessionReaper();
    g_interactive_field_reaper_cv.notify_all();
}

uint64_t interactiveFieldElapsedMs(const InteractiveFieldSession& session) {
    const auto stop = session.state == InteractiveFieldState::Running
        ? std::chrono::steady_clock::now()
        : session.finished;
    return static_cast<uint64_t>(std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(
        stop - session.started).count()));
}

void releaseInteractiveFieldBuffersLocked(InteractiveFieldSession& session) {
    session.field = compute::FieldOutput{};
    std::vector<uint8_t>().swap(session.published);
    session.reservedBytes = 0;
}

void releaseInteractiveFieldResultReader(const std::shared_ptr<InteractiveFieldSession>& session) {
    if (!session) return;
    bool needsPrune = false;
    {
        std::lock_guard<std::mutex> lk(session->mu);
        if (session->resultReaders != 0) --session->resultReaders;
        // A browser acknowledgment may arrive while another result response is
        // still encoding. The last reader, not the ack handler, releases the raw
        // field in that case so the reservation is truthful at every instant.
        if (session->resultDelivered && session->resultReaders == 0) {
            releaseInteractiveFieldBuffersLocked(*session);
            needsPrune = true;
        }
    }
    if (needsPrune) notifyInteractiveFieldSessionReaper();
}

void publishInteractiveFieldRect(const std::shared_ptr<InteractiveFieldSession>& session,
                                 int x, int y, int width, int height) {
    if (!session || width <= 0 || height <= 0) return;
    std::lock_guard<std::mutex> lk(session->mu);
    if (session->state != InteractiveFieldState::Running) return;
    const int fullW = session->params.width;
    const int fullH = session->params.height;
    if (x < 0 || y < 0 || x + width > fullW || y + height > fullH) return;
    if (session->field.iter_u32.size() != static_cast<size_t>(fullW) * static_cast<size_t>(fullH) ||
        session->field.norm_f32.size() != static_cast<size_t>(fullW) * static_cast<size_t>(fullH)) return;

    for (int row = y; row < y + height; ++row) {
        const size_t start = static_cast<size_t>(row) * static_cast<size_t>(fullW) + static_cast<size_t>(x);
        for (int col = 0; col < width; ++col) {
            uint8_t& ready = session->published[start + static_cast<size_t>(col)];
            if (!ready) {
                ready = 1;
                ++session->completedPixels;
            }
        }
    }
    ++session->revision;
}

void finishInteractiveFieldSession(const std::shared_ptr<InteractiveFieldSession>& session) {
    auto finalize = [&](InteractiveFieldState state,
                        const compute::MapStats* stats,
                        const std::string* error) {
        {
            std::lock_guard<std::mutex> lk(session->mu);
            if (stats) session->stats = *stats;
            session->state = state;
            if (error) session->error = *error;
            session->finished = std::chrono::steady_clock::now();
            // A cancelled/failed request has no consumer that can promote its
            // partial field, so release the large native buffers immediately.
            if (state != InteractiveFieldState::Completed) {
                releaseInteractiveFieldBuffersLocked(*session);
            }
        }
        {
            std::lock_guard<std::mutex> sessionsLock(g_interactive_field_sessions_mu);
            pruneInteractiveFieldSessionsLocked();
        }
        notifyInteractiveFieldSessionReaper();
    };

    try {
        compute::MapParams params = session->params;
        params.on_field_tile_done = [session](int x, int y, int width, int height) {
            publishInteractiveFieldRect(session, x, y, width, height);
        };
        const compute::MapStats stats = compute::render_map_field(params, session->field);

        if (!session->cancelFlag || !session->cancelFlag->load(std::memory_order_relaxed)) {
            // A non-incremental backend may only publish after its full launch.
            // Publish the same FieldOutput once rather than re-rendering it.
            bool needsWholeFrame = false;
            {
                std::lock_guard<std::mutex> lk(session->mu);
                needsWholeFrame = session->completedPixels <
                    static_cast<uint64_t>(session->params.width) * static_cast<uint64_t>(session->params.height);
            }
            if (needsWholeFrame) {
                publishInteractiveFieldRect(session, 0, 0, session->params.width, session->params.height);
            }
        }

        const InteractiveFieldState finalState = session->cancelFlag && session->cancelFlag->load(std::memory_order_relaxed)
            ? InteractiveFieldState::Cancelled
            : InteractiveFieldState::Completed;
        finalize(finalState, &stats, nullptr);
    } catch (const std::exception& ex) {
        const InteractiveFieldState finalState = (session->cancelFlag && session->cancelFlag->load(std::memory_order_relaxed)) ||
                isCancelledException(ex)
            ? InteractiveFieldState::Cancelled
            : InteractiveFieldState::Failed;
        const std::string error = finalState == InteractiveFieldState::Failed
            ? std::string(ex.what()) : std::string();
        finalize(finalState, nullptr, finalState == InteractiveFieldState::Failed ? &error : nullptr);
    } catch (...) {
        const std::string error = "interactive field render failed";
        finalize(InteractiveFieldState::Failed, nullptr, &error);
    }
}

std::shared_ptr<InteractiveFieldSession> findInteractiveFieldSession(const std::string& sessionId) {
    std::lock_guard<std::mutex> lk(g_interactive_field_sessions_mu);
    const auto it = g_interactive_field_sessions.find(sessionId);
    if (it == g_interactive_field_sessions.end()) return {};
    return it->second;
}

Json interactiveFieldStatusJsonLocked(const InteractiveFieldSession& session) {
    const uint64_t total = static_cast<uint64_t>(session.params.width) *
                           static_cast<uint64_t>(session.params.height);
    const uint64_t elapsed = interactiveFieldElapsedMs(session);
    const bool deadlinePassed = session.state == InteractiveFieldState::Running &&
                                elapsed >= static_cast<uint64_t>(session.slowAfterMs);
    Json out = {
        {"sessionId", session.id},
        {"requestId", session.requestId},
        {"state", interactiveFieldStateName(session.state)},
        {"width", session.params.width},
        {"height", session.params.height},
        {"viewportAspect", compute::map_viewport_aspect(session.params)},
        {"centerRe", session.params.center_re},
        {"centerIm", session.params.center_im},
        {"scale", session.params.scale},
        {"rotationDeg", session.params.rotation_deg},
        {"elapsedMs", elapsed},
        {"slowAfterMs", session.slowAfterMs},
        {"deadlinePassed", deadlinePassed},
        {"presentationPhase", session.state == InteractiveFieldState::Completed ? "full"
            : (deadlinePassed ? "degraded" : "native_wait")},
        {"revision", session.revision},
        {"completedPixels", session.completedPixels},
        {"totalPixels", total},
        {"coverage", total == 0 ? 0.0 : static_cast<double>(session.completedPixels) / static_cast<double>(total)},
    };
    if (session.state == InteractiveFieldState::Completed) {
        out["generatedMs"] = session.stats.elapsed_ms;
        out["scalarUsed"] = session.stats.scalar_used;
        out["engineUsed"] = session.stats.engine_used;
    }
    if (!session.error.empty()) out["error"] = session.error;
    return out;
}

Json interactiveFieldStatusJson(const std::shared_ptr<InteractiveFieldSession>& session) {
    std::lock_guard<std::mutex> lk(session->mu);
    if (session->state == InteractiveFieldState::Running) {
        session->lastAccess = std::chrono::steady_clock::now();
    }
    return interactiveFieldStatusJsonLocked(*session);
}

struct InteractiveFieldStartInput {
    std::string requestId;
    std::string preemptKey;
    long long preemptSeq = 0;
    int slowAfterMs = 450;
    std::string requestFingerprint;
    compute::MapParams params;
    uint64_t reservedBytes = 0;
    std::shared_ptr<void> customVariantLease;
    compute::Colormap previewColorMap = compute::Colormap::ClassicCos;
    bool previewSmooth = false;
};

InteractiveFieldStartInput parseInteractiveFieldStartInput(
    const std::filesystem::path& repoRoot,
    const Json& j,
    const std::function<bool()>& shouldCancel)
{
    InteractiveFieldStartInput in;
    in.requestId = j.value("requestId", std::string(""));
    in.preemptKey = j.value("preemptKey", std::string(""));
    in.preemptSeq = j.value("preemptSeq", 0LL);
    in.slowAfterMs = j.value("slowAfterMs", 450);
    if (in.requestId.empty() || in.preemptKey.empty()) {
        throw HttpError(400, Json{{"error", "requestId and preemptKey are required"}}.dump());
    }
    if (in.preemptKey.size() > MAX_INTERACTIVE_MAP_PREEMPT_KEY_BYTES || in.preemptSeq < 0) {
        throw HttpError(400, Json{{"error", "invalid interactive preempt key or sequence"}}.dump());
    }
    if (in.slowAfterMs < 100 || in.slowAfterMs > 5000) {
        throw HttpError(400, Json{{"error", "invalid slowAfterMs"}}.dump());
    }

    const std::string cReStr = (j.contains("centerReStr") && j["centerReStr"].is_string())
        ? j["centerReStr"].get<std::string>() : std::string();
    const std::string cImStr = (j.contains("centerImStr") && j["centerImStr"].is_string())
        ? j["centerImStr"].get<std::string>() : std::string();
    const double cRe = resolveCenterCoord(cReStr, j.value("centerRe", -0.75));
    const double cIm = resolveCenterCoord(cImStr, j.value("centerIm", 0.0));
    const double scale = j.value("scale", 3.0);
    const double viewportAspect = j.value("viewportAspect", 0.0);
    const int width = j.value("width", 256);
    const int height = j.value("height", 256);
    const int iterations = j.value("iterations", 1024);
    const std::string variantStr = j.value("variant", std::string("mandelbrot"));
    const std::string metricStr = j.value("metric", std::string("escape"));
    if (!(scale > 0.0) || !std::isfinite(scale) ||
        (j.contains("viewportAspect") &&
         (!(viewportAspect > 0.0) || !std::isfinite(viewportAspect))) ||
        width < 1 || width > MAX_MAP_DIM || height < 1 || height > MAX_MAP_DIM ||
        iterations < 1 || iterations > 1000000 || !std::isfinite(cRe) || !std::isfinite(cIm)) {
        throw HttpError(400, Json{{"error", "invalid interactive field parameters"}}.dump());
    }
    if (!interactiveFieldReservationBytes(width, height, in.reservedBytes)) {
        throw HttpError(400, Json{
            {"error", "interactive field resolution exceeds the resumable-session memory limit"},
            {"maxPixels", MAX_INTERACTIVE_FIELD_PIXELS},
        }.dump());
    }
    if (parseMetric(metricStr) != compute::Metric::Escape) {
        throw HttpError(400, Json{{"error", "interactive field sessions currently require metric=escape"}}.dump());
    }
    if (j.value("colorMode", std::string("direct")) != "direct" ||
        j.contains("transitionTheta") || j.contains("transitionThetaMilliDeg") ||
        j.contains("transitionVariants")) {
        throw HttpError(400, Json{{"error", "interactive field sessions require a direct non-transition map"}}.dump());
    }

    const auto vr = resolveVariant(variantStr, repoRoot);
    double bailout = j.contains("bailout") && !j["bailout"].is_null()
        ? j.value("bailout", 2.0)
        : resolvedDefaultBailout(vr);
    const double bailoutSq = bailoutSqFromJson(j, bailout, resolvedDefaultBailoutSq(vr));
    if (j.contains("bailoutSq") && !j["bailoutSq"].is_null() &&
        !(j.contains("bailout") && !j["bailout"].is_null())) {
        bailout = std::sqrt(bailoutSq);
    }
    if (!(bailout > 0.0) || !std::isfinite(bailout) ||
        !(bailoutSq > 0.0) || !std::isfinite(bailoutSq)) {
        throw HttpError(400, Json{{"error", "invalid bailout"}}.dump());
    }

    compute::MapParams& p = in.params;
    p.center_re = cRe;
    p.center_im = cIm;
    p.center_re_str = cReStr;
    p.center_im_str = cImStr;
    p.scale = scale;
    p.viewport_aspect = viewportAspect;
    p.width = width;
    p.height = height;
    p.iterations = iterations;
    p.bailout = bailout;
    p.bailout_sq = bailoutSq;
    p.variant = vr.var;
    p.custom_step_fn = vr.fn;
    in.customVariantLease = vr.custom_lease;
    p.metric = compute::Metric::Escape;
    p.pairwise_cap = j.value("pairwiseCap", 64);
    if (p.pairwise_cap < 1 || p.pairwise_cap > 1000000) {
        throw HttpError(400, Json{{"error", "invalid pairwiseCap"}}.dump());
    }
    p.julia = j.value("julia", false);
    p.julia_re = j.value("juliaRe", 0.0);
    p.julia_im = j.value("juliaIm", 0.0);
    if (!std::isfinite(p.julia_re) || !std::isfinite(p.julia_im)) {
        throw HttpError(400, Json{{"error", "invalid Julia constant"}}.dump());
    }
    p.scalar_type = j.value("scalarType", std::string("auto"));
    p.engine = j.value("engine", std::string("auto"));
    p.rotation_deg = j.value("rotationDeg", 0.0);
    if (!std::isfinite(p.rotation_deg)) p.rotation_deg = 0.0;
    p.should_cancel = shouldCancel;
    in.previewColorMap = parseColormap(j.value("colorMap", std::string("classic_cos")));
    in.previewSmooth = j.value("smooth", false);
    // A repeated client request ID is idempotent only for the exact same
    // native computation and presentation contract.  Do not silently return
    // a field for a different center/size/iteration request sharing a stale
    // view sequence.
    in.requestFingerprint = Json{
        {"centerRe", p.center_re},
        {"centerIm", p.center_im},
        {"centerReStr", p.center_re_str},
        {"centerImStr", p.center_im_str},
        {"scale", p.scale},
        {"viewportAspect", compute::map_viewport_aspect(p)},
        {"width", p.width},
        {"height", p.height},
        {"iterations", p.iterations},
        {"variant", variantStr},
        {"metric", metricStr},
        {"bailout", p.bailout},
        {"bailoutSq", p.bailout_sq},
        {"pairwiseCap", p.pairwise_cap},
        {"julia", p.julia},
        {"juliaRe", p.julia_re},
        {"juliaIm", p.julia_im},
        {"scalarType", p.scalar_type},
        {"engine", p.engine},
        {"rotationDeg", p.rotation_deg},
        {"colorMap", static_cast<int>(in.previewColorMap)},
        {"smooth", in.previewSmooth},
        {"slowAfterMs", in.slowAfterMs},
    }.dump();
    return in;
}

std::string interactiveFieldViewKey(const std::string& preemptKey, long long preemptSeq) {
    return std::to_string(preemptKey.size()) + ":" + preemptKey + ":" + std::to_string(preemptSeq);
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
        const std::vector<compute::TransitionLeg> multiLegs = parseTransitionLegs(j);
        if (multiLegs.empty() && (!compute::variant_supports_axis_transition(fromVariant) ||
            !compute::variant_supports_axis_transition(toVariant))) {
            throw std::runtime_error("transition variants must be quadratic Mandelbrot-family variants");
        }
        compute::MapParams mp = buildMapParams(in, j, bailout, bailoutSq, shouldCancel);
        mp.julia    = in.julia;
        mp.julia_re = in.juliaRe;
        mp.julia_im = in.juliaIm;
        compute::TransitionParams tp;
        tp.base = std::move(mp);
        tp.theta = in.theta;
        tp.theta_milli_deg_set = true;
        tp.theta_milli_deg = in.thetaMilliDeg;
        tp.from_variant = fromVariant;
        tp.to_variant = toVariant;
        tp.multi_legs = multiLegs;
        compute::FieldOutput fo;
        auto stats = compute::render_transition_field(tp, fo);
        throwIfMapRenderCancelled(shouldCancel);
        rendered.image = compute::colorize_field(tp.base, fo, in.colorMode, in.cyclesPerOctave);
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

    compute::MapParams p = buildMapParams(in, j, bailout, bailoutSq, shouldCancel);
    p.variant = vr.var;
    p.custom_step_fn = vr.fn;
    p.julia = in.julia;
    p.julia_re = in.juliaRe;
    p.julia_im = in.juliaIm;

    compute::MapStats stats;
    if (p.metric == compute::Metric::MandelShipAgree) {
        stats = compute::render_map(p, rendered.image);
    } else {
        compute::FieldOutput fo;
        stats = compute::render_map_field(p, fo);
        throwIfMapRenderCancelled(shouldCancel);
        rendered.image = compute::colorize_field(p, fo, in.colorMode, in.cyclesPerOctave);
    }
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
    auto run = runner.createRun(in.stillExport ? "map-export" : "map", body);
    if (in.stillExport) runner.setCancelable(run.id, true);
    const auto runCancelToken = runner.cancelToken(run.id);
    std::function<bool()> shouldCancel = [preemptFlag, runCancelToken]() {
        return (preemptFlag && preemptFlag->load(std::memory_order_relaxed)) ||
               (runCancelToken && runCancelToken->load(std::memory_order_relaxed));
    };
    if (shouldCancel()) {
        runner.setCancelable(run.id, false);
        runner.setStatus(run.id, "cancelled");
        return cancelledMapRenderJson(run.id, in).dump();
    }

    ResourceManager::Lease heavyLeaseRaw;
    if (in.stillExport) {
        std::string conflictLock, activeRunId;
        if (!resourceManager().tryAcquire(run.id, "still_export", {"cuda_heavy", "cpu_heavy"}, heavyLeaseRaw, conflictLock, activeRunId)) {
            runner.setCancelable(run.id, false);
            runner.setStatus(run.id, "failed");
            throw HttpError(409, Json{
                {"error", "heavy render already running"},
                {"activeRunId", activeRunId},
                {"taskType", "still_export"},
                {"resourceLock", conflictLock},
            }.dump());
        }
        runner.setProgress(run.id, Json{
            {"taskType", "map_export"},
            {"stage", "render"},
            {"current", 0},
            {"total", 1},
            {"percent", 0.0},
            {"engine", in.engine},
            {"scalar", in.scalarType},
            {"elapsedMs", runner.runElapsedMs(run.id)},
            {"cancelable", true},
            {"resourceLocks", Json::array({"cuda_heavy", "cpu_heavy"})},
            {"details", Json::object()},
        }.dump());
    }
    auto heavyLease = std::make_shared<ResourceManager::Lease>(std::move(heavyLeaseRaw));

    auto execute = [=, &runner]() mutable -> Json {
        (void)heavyLease;
        runner.setStatus(run.id, "running");
        if (in.stillExport) {
            runner.setProgress(run.id, Json{
                {"taskType", "map_export"},
                {"stage", "render"},
                {"current", 0},
                {"total", 1},
                {"percent", 0.0},
                {"engine", in.engine},
                {"scalar", in.scalarType},
                {"elapsedMs", runner.runElapsedMs(run.id)},
                {"cancelable", true},
                {"resourceLocks", Json::array({"cuda_heavy", "cpu_heavy"})},
                {"details", Json::object()},
            }.dump());
        }

        MapRenderImage rendered;
        std::filesystem::path imagePath;
        try {
            rendered = renderMapImage(repoRoot, in, shouldCancel);
            throwIfMapRenderCancelled(shouldCancel);
            imagePath = std::filesystem::path(run.outputDir) / rendered.artifactName;
            if (in.stillExport) {
                runner.setCancelable(run.id, false);
                runner.setProgress(run.id, Json{
                    {"taskType", "map_export"},
                    {"stage", "png_encode"},
                    {"current", 0},
                    {"total", 1},
                    {"percent", 99.0},
                    {"engine", rendered.engineUsed},
                    {"scalar", rendered.scalarUsed},
                    {"elapsedMs", runner.runElapsedMs(run.id)},
                    {"cancelable", false},
                    {"resourceLocks", Json::array({"cuda_heavy", "cpu_heavy"})},
                    {"details", Json::object()},
                }.dump());
            }
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
            runner.setCancelable(run.id, false);
            runner.setStatus(run.id, "completed");
        } catch (const std::exception& ex) {
            if (isCancelledException(ex)) {
                if (in.stillExport) {
                    runner.setProgress(run.id, Json{
                        {"taskType", "map_export"},
                        {"stage", "cancelled"},
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
                runner.setCancelable(run.id, false);
                runner.setStatus(run.id, "cancelled");
                return cancelledMapRenderJson(run.id, in);
            }
            if (in.stillExport) {
                runner.setProgress(run.id, Json{
                    {"taskType", "map_export"},
                    {"stage", "failed"},
                    {"current", 0},
                    {"total", 1},
                    {"percent", 0.0},
                    {"engine", in.engine},
                    {"scalar", in.scalarType},
                    {"elapsedMs", runner.runElapsedMs(run.id)},
                    {"cancelable", false},
                    {"resourceLocks", Json::array({"cuda_heavy", "cpu_heavy"})},
                    {"errorMessage", ex.what()},
                    {"details", Json::object()},
                }.dump());
            }
            runner.setCancelable(run.id, false);
            runner.setStatus(run.id, "failed");
            throw;
        }

        const std::string artifactId = run.id + ":" + rendered.artifactName;
        return Json{
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
    };

    const bool background = in.stillExport && in.j.value("background", false);
    if (background) {
        runner.setProgress(run.id, Json{
            {"taskType", "map_export"},
            {"stage", "queued"},
            {"current", 0},
            {"total", 1},
            {"percent", 0.0},
            {"engine", in.engine},
            {"scalar", in.scalarType},
            {"elapsedMs", runner.runElapsedMs(run.id)},
            {"cancelable", true},
            {"resourceLocks", Json::array({"cuda_heavy", "cpu_heavy"})},
            {"details", Json::object()},
        }.dump());
        auto backgroundToken = runner.backgroundTaskToken();
        std::thread([execute, backgroundToken]() mutable {
            (void)backgroundToken;
            try {
                (void)execute();
            } catch (...) {}
        }).detach();
        return Json{
            {"runId", run.id},
            {"status", "queued"},
            {"localExport", in.localExport},
            {"width", in.width},
            {"height", in.height},
            {"requestId", in.requestId},
            {"effective", Json{{"centerRe", in.cRe}, {"centerIm", in.cIm}, {"scale", in.scale}}},
        }.dump();
    }

    return execute().dump();
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
    if (preemptKey.size() > MAX_INTERACTIVE_MAP_PREEMPT_KEY_BYTES || preemptSeq < 0) {
        throw HttpError(400, Json{{"error", "invalid interactive preempt key or sequence"}}.dump());
    }
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

    const std::string requestId = j.value("requestId", std::string(""));
    const std::string preemptKey = j.value("preemptKey", std::string(""));
    const long long preemptSeq = j.value("preemptSeq", 0LL);
    const std::string cReStr = (j.contains("centerReStr") && j["centerReStr"].is_string())
                               ? j["centerReStr"].get<std::string>() : std::string();
    const std::string cImStr = (j.contains("centerImStr") && j["centerImStr"].is_string())
                               ? j["centerImStr"].get<std::string>() : std::string();
    const double cRe     = resolveCenterCoord(cReStr, j.value("centerRe",  -0.75));
    const double cIm     = resolveCenterCoord(cImStr, j.value("centerIm",   0.0));
    const double scale   = j.value("scale",      3.0);
    const double viewportAspect = j.value("viewportAspect", 0.0);
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
    if (j.contains("viewportAspect") &&
        (!(viewportAspect > 0.0) || !std::isfinite(viewportAspect))) {
        throw std::runtime_error("invalid viewportAspect");
    }
    if (width  < 1 || width  > MAX_MAP_DIM)               throw std::runtime_error("invalid width");
    if (height < 1 || height > MAX_MAP_DIM)               throw std::runtime_error("invalid height");
    if (iters  < 1 || iters  > 1000000)            throw std::runtime_error("invalid iterations");
    if (!std::isfinite(cRe) || !std::isfinite(cIm)) throw std::runtime_error("invalid center");

    const bool interactivePreempt = !preemptKey.empty() && !requestId.empty();
    auto preemptFlag = interactivePreempt
        ? registerInteractiveMapRequest(preemptKey, preemptSeq)
        : std::shared_ptr<std::atomic<bool>>{};
    std::function<bool()> shouldCancel = [preemptFlag]() {
        return preemptFlag && preemptFlag->load(std::memory_order_relaxed);
    };
    if (shouldCancel()) {
        return Json{{"status", "cancelled"}, {"requestId", requestId}}.dump();
    }

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
    p.viewport_aspect = viewportAspect;
    p.center_re_str = cReStr;
    p.center_im_str = cImStr;
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
    p.rotation_deg = j.value("rotationDeg", 0.0);
    if (!std::isfinite(p.rotation_deg)) p.rotation_deg = 0.0;
    p.should_cancel = shouldCancel;

    compute::FieldOutput fo;
    compute::MapStats stats;
    try {
        stats = compute::render_map_field(p, fo);
        throwIfMapRenderCancelled(shouldCancel);
    } catch (const std::exception& ex) {
        if (isCancelledException(ex)) {
            return Json{{"status", "cancelled"}, {"requestId", requestId}}.dump();
        }
        throw;
    }

    Json resp = {
        {"status",      "completed"},
        {"requestId",   requestId},
        {"width",       width},
        {"height",      height},
        {"viewportAspect", compute::map_viewport_aspect(p)},
        {"metric",      metricStr},
        {"generatedMs", stats.elapsed_ms},
        {"scalarUsed",  stats.scalar_used},
        {"engineUsed",  stats.engine_used},
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

std::string mapFieldSessionStartRoute(const std::filesystem::path& repoRoot, const std::string& body) {
    const Json j = parseJsonBody(body);
    InteractiveFieldStartInput input = parseInteractiveFieldStartInput(repoRoot, j, {});
    const std::string viewKey = interactiveFieldViewKey(input.preemptKey, input.preemptSeq);
    ensureInteractiveFieldSessionReaper();

    std::shared_ptr<InteractiveFieldSession> session;
    bool startWorker = false;
    {
        std::lock_guard<std::mutex> sessionsLock(g_interactive_field_sessions_mu);
        pruneInteractiveFieldSessionsLocked();
        const auto existing = g_interactive_field_by_view.find(viewKey);
        if (existing != g_interactive_field_by_view.end()) {
            const auto sessionIt = g_interactive_field_sessions.find(existing->second);
            if (sessionIt != g_interactive_field_sessions.end()) {
                session = sessionIt->second;
                std::lock_guard<std::mutex> sessionLock(session->mu);
                if (session->requestId != input.requestId ||
                    session->requestFingerprint != input.requestFingerprint) {
                    throw HttpError(409, Json{{"error", "conflicting interactive field request for view sequence"}}.dump());
                }
            } else {
                g_interactive_field_by_view.erase(existing);
            }
        }

        if (!session) {
            const auto cancelFlag = registerInteractiveMapRequest(input.preemptKey, input.preemptSeq);
            if (cancelFlag->load(std::memory_order_relaxed)) {
                return Json{
                    {"requestId", input.requestId},
                    {"state", "cancelled"},
                    {"status", "cancelled"},
                }.dump();
            }
            makeRoomForInteractiveFieldLocked();
            trimDeliveredInteractiveFieldsForBudgetLocked(input.reservedBytes);
            const size_t liveWorkers = interactiveFieldRunningCountLocked();
            const uint64_t reservedBytes = interactiveFieldReservedBytesLocked();
            const bool workersBusy = liveWorkers >= MAX_RUNNING_INTERACTIVE_FIELDS;
            const bool memoryBusy = reservedBytes >
                MAX_INTERACTIVE_FIELD_RESERVED_BYTES - input.reservedBytes;
            if (workersBusy || memoryBusy) {
                // The client keeps its transformed/current canvas while it
                // retries this exact request ID. No worker is detached here:
                // cancelled workers retain their slot and reservation until
                // their cooperative cancellation has genuinely completed.
                throw HttpError(429, Json{
                    {"error", workersBusy
                        ? "interactive field worker is winding down"
                        : "interactive field memory is temporarily reserved"},
                    {"retryable", true},
                    {"retryAfterMs", INTERACTIVE_FIELD_RETRY_AFTER_MS},
                    {"requestId", input.requestId},
                    {"runningWorkers", liveWorkers},
                    {"reservedBytes", reservedBytes},
                }.dump());
            }

            // The session must hold the exact flag registered above.  A later
            // real viewport change advances the preempt sequence and flips it;
            // status/snapshot polling never re-registers or cancels this work.
            input.params.should_cancel = [cancelFlag]() {
                return cancelFlag && cancelFlag->load(std::memory_order_relaxed);
            };

            session = std::make_shared<InteractiveFieldSession>();
            session->id = "map-field-" + std::to_string(g_interactive_field_session_serial.fetch_add(1));
            session->viewKey = viewKey;
            session->requestId = input.requestId;
            session->requestFingerprint = input.requestFingerprint;
            session->preemptKey = input.preemptKey;
            session->cancelFlag = cancelFlag;
            session->customVariantLease = std::move(input.customVariantLease);
            session->params = std::move(input.params);
            session->previewColorMap = input.previewColorMap;
            session->previewSmooth = input.previewSmooth;
            session->slowAfterMs = input.slowAfterMs;
            session->reservedBytes = input.reservedBytes;
            session->published.assign(static_cast<size_t>(session->params.width) *
                                      static_cast<size_t>(session->params.height), 0u);
            g_interactive_field_sessions[session->id] = session;
            g_interactive_field_by_view[viewKey] = session->id;
            startWorker = true;
        }
    }

    if (startWorker) {
        try {
            std::thread([session]() { finishInteractiveFieldSession(session); }).detach();
        } catch (const std::system_error& ex) {
            {
                std::lock_guard<std::mutex> lk(session->mu);
                session->state = InteractiveFieldState::Failed;
                session->error = "unable to start interactive field worker: " + std::string(ex.what());
                session->finished = std::chrono::steady_clock::now();
                releaseInteractiveFieldBuffersLocked(*session);
            }
            {
                std::lock_guard<std::mutex> sessionsLock(g_interactive_field_sessions_mu);
                const auto sessionIt = g_interactive_field_sessions.find(session->id);
                if (sessionIt != g_interactive_field_sessions.end() && sessionIt->second == session) {
                    g_interactive_field_sessions.erase(sessionIt);
                }
                const auto byView = g_interactive_field_by_view.find(viewKey);
                if (byView != g_interactive_field_by_view.end() && byView->second == session->id) {
                    g_interactive_field_by_view.erase(byView);
                }
            }
            throw HttpError(503, Json{{"error", "unable to start interactive field worker"}}.dump());
        }
    }
    Json resp = interactiveFieldStatusJson(session);
    resp["status"] = resp["state"];
    resp["started"] = startWorker;
    return resp.dump();
}

std::string mapFieldSessionStatusRoute(const std::string& body) {
    const Json j = parseJsonBody(body);
    const std::string sessionId = j.value("sessionId", std::string(""));
    const auto session = findInteractiveFieldSession(sessionId);
    if (!session) throw HttpError(404, Json{{"error", "interactive field session not found"}}.dump());
    Json resp = interactiveFieldStatusJson(session);
    resp["status"] = resp["state"];
    return resp.dump();
}

std::string mapFieldSessionSnapshotRoute(const std::string& body) {
    const Json j = parseJsonBody(body);
    const std::string sessionId = j.value("sessionId", std::string(""));
    const auto session = findInteractiveFieldSession(sessionId);
    if (!session) throw HttpError(404, Json{{"error", "interactive field session not found"}}.dump());

    int previewW = j.value("previewWidth", 512);
    int previewH = j.value("previewHeight", 512);
    constexpr int MAX_INTERACTIVE_PREVIEW_DIM = 1024;

    std::vector<uint32_t> iter;
    std::vector<float> norm;
    std::vector<int> owner;
    compute::Colormap colorMap = compute::Colormap::ClassicCos;
    bool smooth = false;
    int maxIter = 0;
    bool hasPublishedSample = false;
    bool fieldAllocated = false;
    Json resp;
    {
        std::lock_guard<std::mutex> lk(session->mu);
        if (session->state == InteractiveFieldState::Running) {
            session->lastAccess = std::chrono::steady_clock::now();
        }
        const int fullW = session->params.width;
        const int fullH = session->params.height;
        previewW = std::min(previewW, std::min(fullW, MAX_INTERACTIVE_PREVIEW_DIM));
        previewH = std::min(previewH, std::min(fullH, MAX_INTERACTIVE_PREVIEW_DIM));
        if (previewW < 16 || previewH < 16) {
            throw HttpError(400, Json{{"error", "invalid preview dimensions"}}.dump());
        }
        // The renderer allocates FieldOutput just before it begins its first
        // tile. Do not even inspect vector metadata until that tile has
        // published under this mutex; otherwise a snapshot racing allocation
        // would read a vector while another thread mutates it.
        const bool hasPublishedRect = session->revision != 0 && session->completedPixels != 0;
        fieldAllocated = hasPublishedRect &&
                         session->field.iter_u32.size() == static_cast<size_t>(fullW) * static_cast<size_t>(fullH) &&
                         session->field.norm_f32.size() == static_cast<size_t>(fullW) * static_cast<size_t>(fullH);
        if (fieldAllocated) {
            const size_t n = static_cast<size_t>(previewW) * static_cast<size_t>(previewH);
            iter.assign(n, 0u);
            norm.assign(n, 0.0f);
            owner.assign(n, -1);
            for (int y = 0; y < previewH; ++y) {
                const int sy = std::min(fullH - 1, static_cast<int>(
                    (static_cast<int64_t>(2 * y + 1) * fullH) / (2 * previewH)));
                for (int x = 0; x < previewW; ++x) {
                    const int sx = std::min(fullW - 1, static_cast<int>(
                        (static_cast<int64_t>(2 * x + 1) * fullW) / (2 * previewW)));
                    const size_t source = static_cast<size_t>(sy) * static_cast<size_t>(fullW) + static_cast<size_t>(sx);
                    const size_t target = static_cast<size_t>(y) * static_cast<size_t>(previewW) + static_cast<size_t>(x);
                    if (source < session->published.size() && session->published[source]) {
                        owner[target] = static_cast<int>(target);
                        iter[target] = session->field.iter_u32[source];
                        norm[target] = session->field.norm_f32[source];
                        hasPublishedSample = true;
                    }
                }
            }
        }
        colorMap = session->previewColorMap;
        smooth = session->previewSmooth;
        maxIter = session->params.iterations;
        // Keep metadata and pixels from one publication revision. Returning a
        // partial snapshot labelled as a completed/full field is misleading
        // and makes the browser skip its proper final-result promotion.
        resp = interactiveFieldStatusJsonLocked(*session);
    }

    resp["status"] = resp["state"];
    // Palette/smoothing are presentation-only. Let the browser update a slow
    // preview without invalidating the still-running native field session.
    if (j.contains("colorMap") && j["colorMap"].is_string()) {
        colorMap = parseColormap(j["colorMap"].get<std::string>());
    }
    if (j.contains("smooth") && j["smooth"].is_boolean()) {
        smooth = j["smooth"].get<bool>();
    }
    resp["previewWidth"] = previewW;
    resp["previewHeight"] = previewH;
    resp["previewAvailable"] = fieldAllocated && hasPublishedSample;
    if (!fieldAllocated || !hasPublishedSample) return resp.dump();

    // Fill unknown display samples from their nearest published sample. This
    // only changes presentation: every source value came from the one native
    // field render that remains alive in the session.
    std::vector<int> queue;
    queue.reserve(owner.size());
    for (size_t i = 0; i < owner.size(); ++i) {
        if (owner[i] >= 0) queue.push_back(static_cast<int>(i));
    }
    for (size_t head = 0; head < queue.size(); ++head) {
        const int current = queue[head];
        const int x = current % previewW;
        const int y = current / previewW;
        const int neighbours[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
        for (const auto& neighbour : neighbours) {
            const int nx = neighbour[0];
            const int ny = neighbour[1];
            if (nx < 0 || ny < 0 || nx >= previewW || ny >= previewH) continue;
            const size_t ni = static_cast<size_t>(ny) * static_cast<size_t>(previewW) + static_cast<size_t>(nx);
            if (owner[ni] >= 0) continue;
            owner[ni] = owner[static_cast<size_t>(current)];
            queue.push_back(static_cast<int>(ni));
        }
    }

    std::vector<uint8_t> rgba(owner.size() * 4u, 255u);
    for (size_t i = 0; i < owner.size(); ++i) {
        const int source = owner[i];
        uint8_t b = 0, g = 0, r = 0;
        compute::colorize_escape_bgr(
            static_cast<int>(iter[static_cast<size_t>(source)]), maxIter, colorMap,
            static_cast<double>(norm[static_cast<size_t>(source)]), smooth, b, g, r);
        rgba[4u * i] = r;
        rgba[4u * i + 1u] = g;
        rgba[4u * i + 2u] = b;
    }
    resp["rgbaB64"] = base64Encode(rgba.data(), rgba.size());
    return resp.dump();
}

std::string mapFieldSessionResultRoute(const std::string& body) {
    const Json j = parseJsonBody(body);
    const std::string sessionId = j.value("sessionId", std::string(""));
    std::shared_ptr<InteractiveFieldSession> session;
    Json resp;
    std::string iterB64;
    std::string normB64;
    {
        // Lookup, completed-state validation, and reader pin are one
        // registry→session critical section. The reaper takes the same lock
        // order, so it cannot remove an unpinned raw field between lookup and
        // the first base64 reader becoming visible to its reservation scan.
        std::lock_guard<std::mutex> sessionsLock(g_interactive_field_sessions_mu);
        const auto sessionIt = g_interactive_field_sessions.find(sessionId);
        if (sessionIt == g_interactive_field_sessions.end()) {
            throw HttpError(404, Json{{"error", "interactive field session not found"}}.dump());
        }
        session = sessionIt->second;
        std::lock_guard<std::mutex> lk(session->mu);
        if (session->state == InteractiveFieldState::Running) {
            session->lastAccess = std::chrono::steady_clock::now();
        }
        resp = {
            {"sessionId", session->id},
            {"requestId", session->requestId},
            {"status", interactiveFieldStateName(session->state)},
            {"state", interactiveFieldStateName(session->state)},
        };
        if (session->state != InteractiveFieldState::Completed) {
            if (!session->error.empty()) resp["error"] = session->error;
            return resp.dump();
        }
        if (session->resultDelivered) {
            throw HttpError(410, Json{{"error", "interactive field result was already acknowledged"}}.dump());
        }
        const size_t expected = static_cast<size_t>(session->params.width) *
                                static_cast<size_t>(session->params.height);
        if (session->field.iter_u32.size() != expected ||
            session->field.norm_f32.size() != expected) {
            throw HttpError(500, Json{{"error", "interactive field result buffers are unavailable"}}.dump());
        }
        ++session->resultReaders;
        resp["width"] = session->params.width;
        resp["height"] = session->params.height;
        resp["metric"] = "escape";
        resp["maxIter"] = session->params.iterations;
        resp["generatedMs"] = session->stats.elapsed_ms;
        resp["scalarUsed"] = session->stats.scalar_used;
        resp["engineUsed"] = session->stats.engine_used;
    }

    // Pin the raw field before contending on the global transport gate. The
    // reaper sees resultReaders from this point onward, so it cannot erase a
    // completed registry entry in the small hand-off window before encoding.
    bool expectedEncoding = false;
    if (!g_interactive_field_result_encoding.compare_exchange_strong(
            expectedEncoding, true, std::memory_order_acq_rel)) {
        releaseInteractiveFieldResultReader(session);
        throw HttpError(429, Json{
            {"error", "another interactive field result is being prepared"},
            {"retryable", true},
            {"retryAfterMs", INTERACTIVE_FIELD_RETRY_AFTER_MS},
        }.dump());
    }
    struct ResultEncodingLease {
        ~ResultEncodingLease() {
            g_interactive_field_result_encoding.store(false, std::memory_order_release);
        }
    } encodingLease;

    // Completed FieldOutput is immutable. The reader guard keeps its raw
    // reservation live while base64 reads it without blocking snapshots or
    // session admission on session->mu. The browser sends /ack only after it
    // has decoded the completed response.
    try {
        iterB64 = base64Encode(
            reinterpret_cast<const uint8_t*>(session->field.iter_u32.data()),
            session->field.iter_u32.size() * sizeof(uint32_t));
        normB64 = base64Encode(
            reinterpret_cast<const uint8_t*>(session->field.norm_f32.data()),
            session->field.norm_f32.size() * sizeof(float));
    } catch (...) {
        releaseInteractiveFieldResultReader(session);
        throw;
    }
    releaseInteractiveFieldResultReader(session);
    resp["iterB64"] = std::move(iterB64);
    resp["finalMagB64"] = std::move(normB64);
    return resp.dump();
}

std::string mapFieldSessionAcknowledgeRoute(const std::string& body) {
    const Json j = parseJsonBody(body);
    const std::string sessionId = j.value("sessionId", std::string(""));
    const std::string requestId = j.value("requestId", std::string(""));
    std::shared_ptr<InteractiveFieldSession> session;
    Json resp;
    {
        std::lock_guard<std::mutex> sessionsLock(g_interactive_field_sessions_mu);
        const auto sessionIt = g_interactive_field_sessions.find(sessionId);
        if (sessionIt == g_interactive_field_sessions.end()) {
            throw HttpError(404, Json{{"error", "interactive field session not found"}}.dump());
        }
        session = sessionIt->second;
        std::lock_guard<std::mutex> lk(session->mu);
        if (requestId.empty() || requestId != session->requestId) {
            throw HttpError(409, Json{{"error", "interactive field acknowledgement request mismatch"}}.dump());
        }
        if (session->state == InteractiveFieldState::Completed) {
            session->resultDelivered = true;
            session->lastAccess = std::chrono::steady_clock::now();
            if (session->resultReaders == 0) {
                releaseInteractiveFieldBuffersLocked(*session);
            }
        }
        resp = {
            {"sessionId", session->id},
            {"requestId", session->requestId},
            {"status", interactiveFieldStateName(session->state)},
            {"state", interactiveFieldStateName(session->state)},
            {"resultAcknowledged", session->resultDelivered},
        };
        if (!session->error.empty()) resp["error"] = session->error;
    }
    notifyInteractiveFieldSessionReaper();
    return resp.dump();
}

} // namespace fsd
