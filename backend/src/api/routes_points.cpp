// routes_points.cpp — native Newton solver endpoint.

#include "routes.hpp"
#include "routes_common.hpp"
#include "resource_manager.hpp"

#include "../compute/newton/mandelbrot_sp.hpp"
#include "../compute/special_points.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace fsd {

namespace {

constexpr int kLocalCenterMaxPeriod = 16384;
constexpr int kLocalCenterMaxAttempts = 8192;
constexpr int kLocalMisiurewiczMaxPreperiod = 4096;
constexpr int kLocalMisiurewiczMaxPeriod = 4096;
constexpr int kLocalMisiurewiczMaxSum = 8192;
constexpr int kLocalMisiurewiczMaxPairs = 16384;

struct SpecialPointSearchPreemptEntry {
    long long seq = -1;
    std::weak_ptr<std::atomic<bool>> cancelFlag;
};

std::mutex gSpecialPointSearchPreemptMu;
std::unordered_map<std::string, SpecialPointSearchPreemptEntry> gSpecialPointSearchPreempt;
std::weak_ptr<std::atomic<bool>> gLatestAcceptedSpecialPointSearch;

class RunLaunchGuard {
public:
    RunLaunchGuard(JobRunner& runner, std::string runId)
        : runner_(runner), runId_(std::move(runId)) {}
    ~RunLaunchGuard() {
        if (!armed_) return;
        try { runner_.setStatus(runId_, "failed"); } catch (...) {}
    }
    void release() { armed_ = false; }

private:
    JobRunner& runner_;
    std::string runId_;
    bool armed_ = true;
};

std::shared_ptr<std::atomic<bool>> registerSpecialPointSearch(
    const std::string& key,
    long long seq
) {
    auto flag = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard<std::mutex> lock(gSpecialPointSearchPreemptMu);

    if (gSpecialPointSearchPreempt.size() > 256) {
        for (auto it = gSpecialPointSearchPreempt.begin(); it != gSpecialPointSearchPreempt.end();) {
            if (it->second.cancelFlag.expired()) it = gSpecialPointSearchPreempt.erase(it);
            else ++it;
        }
    }

    auto& entry = gSpecialPointSearchPreempt[key.empty() ? "legacy" : key];
    if (seq >= entry.seq) {
        if (auto previous = entry.cancelFlag.lock()) {
            previous->store(true, std::memory_order_relaxed);
        }
        entry.seq = seq;
        entry.cancelFlag = flag;

        // The studio has one CPU-heavy solver slot. A request that is current
        // for its client also supersedes the globally accepted search, so
        // remounts/tabs cannot leave an unbounded queue behind the resource
        // lease. Stale same-client requests never reach this step.
        if (auto previous = gLatestAcceptedSpecialPointSearch.lock()) {
            previous->store(true, std::memory_order_relaxed);
        }
        gLatestAcceptedSpecialPointSearch = flag;
    } else {
        flag->store(true, std::memory_order_relaxed);
    }
    return flag;
}

Json orbitToJson(const compute::OrbitClassification& o) {
    const std::string kind = o.is_center ? "center" : (o.is_misiurewicz ? "misiurewicz" : "unknown");
    return Json{
        {"kind", kind},
        {"found_repeat", o.found_repeat},
        {"is_center", o.is_center},
        {"is_misiurewicz", o.is_misiurewicz},
        {"preperiod", o.preperiod},
        {"period", o.period},
        {"repeat_error", o.repeat_error},
    };
}

Json variantsToJson(const std::vector<compute::VariantExistence>& variants) {
    Json arr = Json::array();
    for (const auto& v : variants) {
        arr.push_back({
            {"variant_name", v.variant_name},
            {"exists", v.exists},
            {"same_orbit_as_mandelbrot", v.same_orbit_as_mandelbrot},
            {"actual_preperiod", v.actual_preperiod},
            {"actual_period", v.actual_period},
            {"repeat_error", v.repeat_error},
            {"reason", v.reason},
        });
    }
    return arr;
}

Json compatibleVariantsToJson(const std::vector<compute::VariantExistence>& variants) {
    Json arr = Json::array();
    for (const auto& v : variants) {
        if (v.exists && v.same_orbit_as_mandelbrot) arr.push_back(v.variant_name);
    }
    return arr;
}

Json variantCompatibilityToJson(const std::vector<compute::VariantExistence>& variants) {
    Json obj = Json::object();
    for (const auto& v : variants) {
        obj[v.variant_name] = {
            {"compatible", v.exists && v.same_orbit_as_mandelbrot},
            {"exists", v.exists},
            {"sameOrbitAsMandelbrot", v.same_orbit_as_mandelbrot},
            {"actualPreperiod", v.actual_preperiod},
            {"actualPeriod", v.actual_period},
            {"repeatError", v.repeat_error},
            {"reason", v.reason},
        };
    }
    return obj;
}

Json pointToJson(const compute::SpecialPointResult& p) {
    Json out{
        {"id", p.id},
        {"kind", compute::special_point_kind_name(p.kind)},
        {"preperiod", p.preperiod},
        {"period", p.period},
        {"requestedPeriod", p.period},
        {"actualPeriod", p.actual.period},
        {"re", p.re},
        {"im", p.im},
        {"real", p.re},
        {"imag", p.im},
        {"converged", p.converged},
        {"success", p.converged},
        {"accepted", p.accepted},
        {"fallback", p.fallback},
        {"visible", p.visible},
        {"residual", p.residual},
        {"newtonIterations", p.newton_iterations},
        {"actual", orbitToJson(p.actual)},
        {"variants", variantsToJson(p.variants)},
        {"compatibleVariants", compatibleVariantsToJson(p.variants)},
        {"variantCompatibility", variantCompatibilityToJson(p.variants)},
        {"reason", p.reason},
    };
    if (!p.re_str.empty() && !p.im_str.empty()) {
        out["reStr"] = p.re_str;
        out["imStr"] = p.im_str;
        out["precBits"] = p.prec_bits;
    }
    return out;
}

Json enumResponseToJson(const compute::SpecialPointEnumResponse& r) {
    Json points = Json::array();
    for (const auto& p : r.points) points.push_back(pointToJson(p));
    Json rejected = Json::array();
    for (const auto& p : r.rejected_debug) rejected.push_back(pointToJson(p));
    return Json{
        {"complete", r.complete},
        {"status", r.status},
        {"acceptedCount", r.accepted_count},
        {"expectedCount", r.expected_count},
        {"seedCount", r.seed_count},
        {"newtonSuccessCount", r.newton_success_count},
        {"rejectedCount", r.rejected_count},
        {"points", points},
        {"rejected_debug", rejected},
        {"warning", r.warning},
    };
}

Json searchResponseToJson(const compute::SpecialPointSearchResponse& r) {
    Json points = Json::array();
    for (const auto& p : r.points) points.push_back(pointToJson(p));
    return Json{
        {"status", r.status},
        {"sampled", r.sampled},
        {"foundAny", !r.points.empty()},
        {"noPoint", r.status == "completed" && r.points.empty()},
        {"acceptedCount", r.accepted_count},
        {"fallbackCount", r.fallback_count},
        {"seedCount", r.seed_count},
        {"newtonSuccessCount", r.newton_success_count},
        {"rejectedCount", r.rejected_count},
        {"points", points},
        {"warning", r.warning},
    };
}

compute::SpecialPointKind parseKind(const Json& j) {
    const std::string raw = j.value("kind", std::string("center"));
    if (raw == "center" || raw == "hyperbolic" || raw == "hyperbolic_center") {
        return compute::SpecialPointKind::HyperbolicCenter;
    }
    if (raw == "misiurewicz") return compute::SpecialPointKind::Misiurewicz;
    throw HttpError(400, Json{{"error", "invalid special point kind"}}.dump());
}

compute::SpecialPointViewport parseViewport(const Json& j) {
    compute::SpecialPointViewport viewport;
    if (j.contains("viewport") && j["viewport"].is_object()) {
        const Json& v = j["viewport"];
        viewport.enabled = true;
        viewport.center_re = v.value("centerRe", -0.75);
        viewport.center_im = v.value("centerIm", 0.0);
        viewport.scale = v.value("scale", 3.0);
        viewport.rotation_deg = v.value("rotationDeg", 0.0);
        viewport.width = v.value("width", 1200);
        viewport.height = v.value("height", 800);
        if (v.contains("centerReStr") && v["centerReStr"].is_string())
            viewport.center_re_str = v["centerReStr"].get<std::string>();
        if (v.contains("centerImStr") && v["centerImStr"].is_string())
            viewport.center_im_str = v["centerImStr"].get<std::string>();
    }
    return viewport;
}

bool parsesAsDecimal(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double parsed = std::strtod(s.c_str(), &end);
    return end != s.c_str() && *end == '\0' && std::isfinite(parsed);
}

compute::SpecialPointEnumRequest parseEnumRequest(const Json& j) {
    compute::SpecialPointEnumRequest req;
    req.kind = parseKind(j);
    req.period_min = j.value("periodMin", 1);
    req.period_max = j.value("periodMax", req.kind == compute::SpecialPointKind::HyperbolicCenter ? 8 : 4);
    req.preperiod_min = j.value("preperiodMin", 1);
    req.preperiod_max = j.value("preperiodMax", 4);
    req.misiurewicz_period_min = j.value("periodMin", 1);
    req.misiurewicz_period_max = j.value("periodMax", 4);
    req.max_newton_iter = j.value("maxNewtonIter", 60);
    req.max_seed_batches = j.value("maxSeedBatches", 80);
    req.seeds_per_batch = j.value("seedsPerBatch", 2048);
    req.newton_eps = j.value("newtonEps", 1e-13);
    req.classify_eps = j.value("classifyEps", 1e-10);
    req.root_merge_eps = j.value("rootMergeEps", 1e-9);
    req.include_variant_existence = j.value("includeVariantExistence", true);
    req.include_rejected_debug = j.value("includeRejectedDebug", false);
    req.visible_only = j.value("visibleOnly", false);

    req.viewport = parseViewport(j);
    return req;
}

compute::SpecialPointSearchRequest parseSearchRequest(const Json& j) {
    compute::SpecialPointSearchRequest req;
    req.kind = parseKind(j);
    req.period_min = j.value("periodMin", 1);
    req.period_max = j.value("periodMax", req.kind == compute::SpecialPointKind::HyperbolicCenter
        ? kLocalCenterMaxAttempts
        : 4);
    req.preperiod_min = j.value("preperiodMin", 1);
    req.preperiod_max = j.value("preperiodMax", 4);
    req.seed_budget = j.value("seedBudget", 2000);
    req.max_newton_iter = j.value("maxNewtonIter", 60);
    req.newton_eps = j.value("newtonEps", 1e-13);
    req.classify_eps = j.value("classifyEps", 1e-10);
    req.root_merge_eps = j.value("rootMergeEps", 1e-10);
    req.visible_only = j.value("visibleOnly", true);
    req.include_variant_compatibility = j.value("includeVariantCompatibility", true);
    req.viewport = parseViewport(j);
    return req;
}

int totalExpectedOrThrow(const compute::SpecialPointEnumRequest& req) {
    int total = 0;
    if (req.kind == compute::SpecialPointKind::HyperbolicCenter) {
        for (int p = req.period_min; p <= req.period_max; ++p) {
            const int count = compute::expected_center_count(p);
            if (count < 0) throw HttpError(400, Json{{"error", "expected count unavailable"}}.dump());
            total += count;
        }
    } else {
        for (int m = req.preperiod_min; m <= req.preperiod_max; ++m) {
            for (int p = req.misiurewicz_period_min; p <= req.misiurewicz_period_max; ++p) {
                const int count = compute::expected_misiurewicz_count(m, p);
                if (count < 0) {
                    throw HttpError(400, Json{
                        {"error", "expected count unavailable for requested Misiurewicz parameter"},
                        {"suggestion", "reduce preperiod/period range"},
                    }.dump());
                }
                total += count;
            }
        }
    }
    return total;
}

void validateEnumRequest(const compute::SpecialPointEnumRequest& req) {
    if (req.kind == compute::SpecialPointKind::HyperbolicCenter) {
        if (req.period_min < 1 || req.period_max < req.period_min || req.period_max > 10) {
            throw HttpError(400, Json{{"error", "invalid center period range"}, {"limit", "1..10"}}.dump());
        }
    } else {
        if (req.preperiod_min < 1 || req.preperiod_max < req.preperiod_min || req.preperiod_max > 6 ||
            req.misiurewicz_period_min < 1 || req.misiurewicz_period_max < req.misiurewicz_period_min ||
            req.misiurewicz_period_max > 6 ||
            req.preperiod_max + req.misiurewicz_period_max > 10) {
            throw HttpError(400, Json{{"error", "invalid Misiurewicz range"}, {"limit", "preperiod 1..6, period 1..6, preperiod+period <= 10"}}.dump());
        }
    }
    if (req.max_newton_iter < 1 || req.max_newton_iter > 80 ||
        req.max_seed_batches < 1 || req.max_seed_batches > 200 ||
        req.seeds_per_batch < 1 || req.seeds_per_batch > 10000) {
        throw HttpError(400, Json{{"error", "invalid solver limits"}}.dump());
    }
    const int expected = totalExpectedOrThrow(req);
    if (expected > 3000) {
        throw HttpError(400, Json{
            {"error", "parameter range too large"},
            {"expectedCount", expected},
            {"limit", 3000},
            {"suggestion", "reduce period/preperiod range"},
        }.dump());
    }
}

void validateSearchRequest(const compute::SpecialPointSearchRequest& req) {
    if (req.kind == compute::SpecialPointKind::HyperbolicCenter) {
        if (req.period_min < 1 || req.period_max < req.period_min || req.period_max > kLocalCenterMaxPeriod) {
            throw HttpError(400, Json{
                {"error", "invalid center period range"},
                {"limit", std::string("1..") + std::to_string(kLocalCenterMaxPeriod)},
            }.dump());
        }
        const int centerAttempts = req.period_max - req.period_min + 1;
        if (centerAttempts > kLocalCenterMaxAttempts) {
            throw HttpError(400, Json{
                {"error", "local center search range too large"},
                {"limit", kLocalCenterMaxAttempts},
                {"suggestion", "narrow periodMin/periodMax for this local solve"},
            }.dump());
        }
    } else {
        if (req.preperiod_min < 1 || req.preperiod_max < req.preperiod_min || req.preperiod_max > kLocalMisiurewiczMaxPreperiod ||
            req.period_min < 1 || req.period_max < req.period_min || req.period_max > kLocalMisiurewiczMaxPeriod ||
            req.preperiod_max + req.period_max > kLocalMisiurewiczMaxSum) {
            throw HttpError(400, Json{
                {"error", "invalid Misiurewicz search range"},
                {"limit", "preperiod 1..4096, period 1..4096, preperiod+period <= 8192"},
            }.dump());
        }
        const int64_t taskCount =
            static_cast<int64_t>(req.preperiod_max - req.preperiod_min + 1) *
            static_cast<int64_t>(req.period_max - req.period_min + 1);
        if (taskCount > kLocalMisiurewiczMaxPairs) {
            throw HttpError(400, Json{
                {"error", "local Misiurewicz search range too large"},
                {"limit", kLocalMisiurewiczMaxPairs},
                {"suggestion", "narrow preperiod/period range for this local solve"},
            }.dump());
        }
    }
    if (req.max_newton_iter < 1 || req.max_newton_iter > 80) {
        throw HttpError(400, Json{{"error", "invalid maxNewtonIter"}, {"limit", "1..80"}}.dump());
    }
    if (!req.viewport.enabled || req.viewport.width < 1 || req.viewport.height < 1 ||
        !std::isfinite(req.viewport.center_re) || !std::isfinite(req.viewport.center_im) ||
        !std::isfinite(req.viewport.scale) || req.viewport.scale <= 0.0 ||
        !std::isfinite(req.viewport.rotation_deg)) {
        throw HttpError(400, Json{{"error", "valid viewport required"}}.dump());
    }
    if ((!req.viewport.center_re_str.empty() && !parsesAsDecimal(req.viewport.center_re_str)) ||
        (!req.viewport.center_im_str.empty() && !parsesAsDecimal(req.viewport.center_im_str))) {
        throw HttpError(400, Json{{"error", "invalid viewport center strings"}}.dump());
    }
}

std::string readRunJsonFile(JobRunner& runner, const std::string& runId, const std::string& fileName) {
    const std::string outDir = runner.resolveOutputDir(runId);
    if (outDir.empty()) throw HttpError(404, Json{{"error", "run not found"}}.dump());
    std::ifstream is(std::filesystem::path(outDir) / fileName);
    if (!is) throw HttpError(404, Json{{"error", "special point results not found"}}.dump());
    return std::string(std::istreambuf_iterator<char>(is), std::istreambuf_iterator<char>());
}

void persistPoints(
    const std::filesystem::path& repoRoot,
    const std::vector<compute::newton_sp::SolvedPoint>& pts,
    const std::string& sourceMode,
    const std::string& pointType,
    Json& out
) {
    Db db = openDb(repoRoot);
    const std::string createdAt = nowIso8601();
    out = Json::array();
    for (const auto& pt : pts) {
        SpecialPointRecord rec;
        rec.id = makeId();
        rec.family = pt.variant;
        rec.pointType = pointType;
        rec.k = pt.k;
        rec.p = pt.p;
        rec.re = pt.c.real();
        rec.im = pt.c.imag();
        rec.sourceMode = sourceMode;
        rec.createdAt = createdAt;
        db.insertSpecialPoint(rec);
        out.push_back({
            {"id", rec.id},
            {"real", rec.re},
            {"imag", rec.im},
            {"k", rec.k},
            {"p", rec.p},
            {"family", rec.family},
        });
    }
}

} // namespace

std::string specialPointsEnumerateRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& body) {
    const Json j = parseJsonBody(body);
    compute::SpecialPointEnumRequest req = parseEnumRequest(j);
    validateEnumRequest(req);

    auto run = runner.createRun("special-points-enumerate", body);
    RunLaunchGuard launchGuard(runner, run.id);
    const bool background = j.value("background", false);
    runner.setCancelable(run.id, true);
    auto cancelToken = runner.cancelToken(run.id);

    ResourceManager::Lease rawLease;
    std::string conflictLock;
    std::string activeRunId;
    if (!resourceManager().tryAcquire(
            run.id,
            "special_points_enumerate",
            {"special_points", "cpu_heavy"},
            rawLease,
            conflictLock,
            activeRunId)) {
        runner.setProgress(run.id, Json{
            {"taskType", "special_points"},
            {"stage", "failed"},
            {"cancelable", true},
            {"resourceLocks", Json::array({"special_points", "cpu_heavy"})},
            {"errorMessage", "special point solver already running"},
        }.dump());
        runner.setStatus(run.id, "failed");
        throw HttpError(409, Json{
            {"error", "special point solver already running"},
            {"activeRunId", activeRunId},
            {"resourceLock", conflictLock},
        }.dump());
    }
    auto lease = std::make_shared<ResourceManager::Lease>(std::move(rawLease));

    auto setProgress = [&runner, run](const std::string& stage, int accepted,
                                     int expected, int seeds, int batch,
                                     bool kernelReported = false) {
        Json progress = {
            {"taskType", "special_points"},
            {"stage", stage},
            {"acceptedCount", accepted},
            {"expectedCount", expected},
            {"seedCount", seeds},
            {"batchIndex", batch},
            {"engine", "openmp"},
            {"scalar", "fp64"},
            {"kernelReported", kernelReported},
            {"elapsedMs", runner.runElapsedMs(run.id)},
            {"cancelable", true},
            {"resourceLocks", Json::array({"special_points", "cpu_heavy"})},
        };
        runner.setProgress(run.id, progress.dump());
    };
    setProgress("queued", 0, totalExpectedOrThrow(req), 0, 0);

    auto execute = [=, &runner]() mutable -> Json {
        (void)lease;
        RunLaunchGuard terminalGuard(runner, run.id);
        runner.setStatus(run.id, "running");
        try {
        setProgress("enumerating", 0, totalExpectedOrThrow(req), 0, 0);
        compute::SpecialPointEnumResponse resp = compute::enumerate_special_points(
            req,
            [&](int taskIndex, int taskCount, int accepted, int expected, int seeds, int batch) {
                Json progress = {
                    {"taskType", "special_points"},
                    {"stage", "enumerating"},
                    {"taskIndex", taskIndex},
                    {"taskCount", taskCount},
                    {"acceptedCount", accepted},
                    {"expectedCount", expected},
                    {"seedCount", seeds},
                    {"batchIndex", batch},
                    {"engine", "openmp"},
                    {"scalar", "fp64"},
                    {"kernelReported", false},
                    {"elapsedMs", runner.runElapsedMs(run.id)},
                    {"cancelable", true},
                    {"resourceLocks", Json::array({"special_points", "cpu_heavy"})},
                };
                runner.setProgress(run.id, progress.dump());
                return !cancelToken->load(std::memory_order_relaxed);
            },
            [cancelToken]() { return cancelToken->load(std::memory_order_relaxed); });

        if (cancelToken->load(std::memory_order_relaxed) || resp.status == "cancelled") {
            runner.setCancelable(run.id, false);
            runner.setStatus(run.id, "cancelled");
            terminalGuard.release();
            return Json{{"runId", run.id}, {"status", "cancelled"}};
        }

        Json responseJson = enumResponseToJson(resp);
        responseJson["runId"] = run.id;

        Json artifact = {
            {"version", 1},
            {"timestamp", nowIso8601()},
            {"request", j},
            {"response", responseJson},
            {"points", responseJson["points"]},
        };
        if (req.include_rejected_debug) artifact["rejected_debug"] = responseJson["rejected_debug"];
        const std::filesystem::path outPath = std::filesystem::path(run.outputDir) / "special_points.json";
        atomicWriteText(outPath, artifact.dump(2));
        runner.addArtifact(run.id, Artifact{"special-points", outPath.string(), "report"});

        setProgress(resp.status, resp.accepted_count, resp.expected_count,
                    resp.seed_count, req.max_seed_batches, true);
        runner.setCancelable(run.id, false);
        runner.setStatus(run.id, "completed");
        terminalGuard.release();
        const std::string artId = run.id + ":special_points.json";
        responseJson["reportArtifactId"] = artId;
        responseJson["reportDownloadUrl"] = "/api/artifacts/download?artifactId=" + artId;
        return responseJson;
        } catch (const std::exception&) {
        runner.setCancelable(run.id, false);
        if (cancelToken->load(std::memory_order_relaxed)) {
            runner.setStatus(run.id, "cancelled");
        } else {
            runner.setStatus(run.id, "failed");
            setProgress("failed", 0, totalExpectedOrThrow(req), 0, 0);
        }
        terminalGuard.release();
        throw;
        }
    };

    launchGuard.release();
    if (background) {
        auto backgroundToken = runner.backgroundTaskToken();
        std::thread([execute, backgroundToken]() mutable {
            (void)backgroundToken;
            try {
                (void)execute();
            } catch (...) {}
        }).detach();
        return Json{
            {"runId", run.id}, {"status", "queued"},
            {"acceptedCount", 0}, {"points", Json::array()},
        }.dump();
    }

    return execute().dump();
}

std::string specialPointsSearchRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& body) {
    (void)repoRoot;
    const Json j = parseJsonBody(body);
    compute::SpecialPointSearchRequest req = parseSearchRequest(j);
    validateSearchRequest(req);
    const std::string preemptKey = j.value("preemptKey", std::string(""));
    const long long preemptSeq = j.value("preemptSeq", 0LL);
    if (preemptKey.size() > 128 || preemptSeq < 0) {
        throw HttpError(400, Json{{"error", "invalid special point preemption token"}}.dump());
    }

    auto run = runner.createRun("special-points-search", body);
    RunLaunchGuard launchGuard(runner, run.id);
    runner.setCancelable(run.id, true);
    auto cancelToken = runner.cancelToken(run.id);
    auto backgroundToken = runner.backgroundTaskToken();
    auto preemptFlag = registerSpecialPointSearch(preemptKey, preemptSeq);

    auto setProgress = [&](const std::string& stage, int period, int accepted, int seeds) {
        Json progress = {
            {"taskType", "special_points_search"},
            {"stage", stage},
            {"period", period},
            {"acceptedCount", accepted},
            {"seedCount", seeds},
            {"engine", "openmp"},
            {"scalar", "fp64"},
            {"kernelReported", false},
            {"elapsedMs", runner.runElapsedMs(run.id)},
            {"cancelable", true},
            {"resourceLocks", Json::array({"special_points", "cpu_heavy"})},
        };
        runner.setProgress(run.id, progress.dump());
    };

    setProgress("queued", req.period_min, 0, 0);

    std::thread([run, req, j, preemptFlag, cancelToken, backgroundToken, &runner]() {
        (void)backgroundToken;
        RunLaunchGuard terminalGuard(runner, run.id);
        auto shouldCancel = [&]() {
            return preemptFlag->load(std::memory_order_relaxed) ||
                cancelToken->load(std::memory_order_relaxed);
        };
        auto setThreadProgress = [&](const std::string& stage, int period, int accepted,
                                     int seeds, const std::string& error = "",
                                     bool kernelReported = false) {
            Json progress = {
                {"taskType", "special_points_search"},
                {"stage", stage},
                {"period", period},
                {"acceptedCount", accepted},
                {"seedCount", seeds},
                {"engine", "openmp"},
                {"scalar", "fp64"},
                {"kernelReported", kernelReported},
                {"elapsedMs", runner.runElapsedMs(run.id)},
                {"cancelable", true},
                {"resourceLocks", Json::array({"special_points", "cpu_heavy"})},
            };
            if (!error.empty()) progress["errorMessage"] = error;
            runner.setProgress(run.id, progress.dump());
        };

        ResourceManager::Lease lease;
        try {
            for (;;) {
                if (shouldCancel()) {
                    setThreadProgress("cancelled", req.period_min, 0, 0);
                    runner.setCancelable(run.id, false);
                    runner.setStatus(run.id, "cancelled");
                    terminalGuard.release();
                    return;
                }
                std::string conflictLock;
                std::string activeRunId;
                if (resourceManager().tryAcquire(
                        run.id,
                        "special_points_search",
                        {"special_points", "cpu_heavy"},
                        lease,
                        conflictLock,
                        activeRunId)) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            if (shouldCancel()) {
                setThreadProgress("cancelled", req.period_min, 0, 0);
                runner.setCancelable(run.id, false);
                runner.setStatus(run.id, "cancelled");
                terminalGuard.release();
                return;
            }
            runner.setStatus(run.id, "running");
            setThreadProgress("searching", req.period_min, 0, 0);

            compute::SpecialPointSearchResponse resp = compute::search_special_points(
                req,
                [&](int period, int periodIndex, int periodCount, int accepted, int seeds) {
                    Json progress = {
                        {"taskType", "special_points_search"},
                        {"stage", "searching"},
                        {"period", period},
                        {"periodIndex", periodIndex},
                        {"periodCount", periodCount},
                        {"acceptedCount", accepted},
                        {"seedCount", seeds},
                        {"engine", "openmp"},
                        {"scalar", "fp64"},
                        {"kernelReported", false},
                        {"elapsedMs", runner.runElapsedMs(run.id)},
                        {"cancelable", true},
                        {"resourceLocks", Json::array({"special_points", "cpu_heavy"})},
                    };
                    runner.setProgress(run.id, progress.dump());
                    return !shouldCancel();
                },
                shouldCancel);

            if (shouldCancel() || resp.status == "cancelled") {
                setThreadProgress("cancelled", req.period_min, resp.accepted_count, resp.seed_count);
                runner.setCancelable(run.id, false);
                runner.setStatus(run.id, "cancelled");
                terminalGuard.release();
                return;
            }

            Json responseJson = searchResponseToJson(resp);
            responseJson["runId"] = run.id;

            Json artifact = {
                {"version", 1},
                {"timestamp", nowIso8601()},
                {"request", j},
                {"response", responseJson},
                {"points", responseJson["points"]},
            };
            const std::filesystem::path outPath = std::filesystem::path(run.outputDir) / "special_points_search.json";
            atomicWriteText(outPath, artifact.dump(2));
            runner.addArtifact(run.id, Artifact{"special-points-search", outPath.string(), "report"});

            setThreadProgress(resp.status, req.period_max, resp.accepted_count,
                              resp.seed_count, "", true);
            runner.setCancelable(run.id, false);
            runner.setStatus(run.id, "completed");
            terminalGuard.release();
        } catch (const std::exception& e) {
            const bool cancelled = shouldCancel();
            try {
                runner.setCancelable(run.id, false);
                runner.setStatus(run.id, cancelled ? "cancelled" : "failed");
                terminalGuard.release();
            } catch (...) {}
            try {
                setThreadProgress(
                    cancelled ? "cancelled" : "failed",
                    req.period_min,
                    0,
                    0,
                    cancelled ? "" : e.what());
            } catch (...) {}
        } catch (...) {
            try {
                runner.setCancelable(run.id, false);
                runner.setStatus(run.id, shouldCancel() ? "cancelled" : "failed");
                terminalGuard.release();
            } catch (...) {}
        }
    }).detach();
    launchGuard.release();

    return Json{
        {"runId", run.id},
        {"status", "queued"},
        {"sampled", false},
        {"acceptedCount", 0},
        {"fallbackCount", 0},
        {"seedCount", 0},
        {"newtonSuccessCount", 0},
        {"rejectedCount", 0},
        {"points", Json::array()},
        {"warning", "viewport local solve queued"},
    }.dump();
}

std::string specialPointsResultsRoute(const std::filesystem::path&, JobRunner& runner, const std::string& query) {
    const std::string runId = getQueryParam(query, "runId");
    if (runId.empty()) throw HttpError(400, Json{{"error", "runId required"}}.dump());
    try {
        const Json artifact = Json::parse(readRunJsonFile(runner, runId, "special_points_search.json"));
        Json response = artifact.value("response", Json::object());
        response["runId"] = runId;
        return response.dump();
    } catch (const HttpError&) {
        try {
            const Json artifact = Json::parse(readRunJsonFile(runner, runId, "special_points.json"));
            Json response = artifact.value("response", Json::object());
            response["runId"] = runId;
            return response.dump();
        } catch (const HttpError&) {
            Json progress = Json::object();
            try { progress = Json::parse(runner.getProgress(runId)); } catch (...) {}
            RunRecord run = runner.getRun(runId);
            return Json{
                {"runId", runId},
                {"status", run.status},
                {"sampled", false},
                {"foundAny", false},
                {"noPoint", false},
                {"acceptedCount", progress.value("acceptedCount", 0)},
                {"fallbackCount", 0},
                {"seedCount", progress.value("seedCount", 0)},
                {"newtonSuccessCount", 0},
                {"rejectedCount", 0},
                {"points", Json::array()},
                {"progress", progress},
                {"warning", "viewport local solve running"},
            }.dump();
        }
    }
}

std::string specialPointsSnapRoute(const std::string& body) {
    const Json j = parseJsonBody(body);
    compute::SpecialPointSearchRequest req;
    req.kind = compute::SpecialPointKind::HyperbolicCenter;
    req.period_min = j.value("period", j.value("requestedPeriod", 1));
    req.period_max = req.period_min;
    req.max_newton_iter = j.value("maxNewtonIter", 60);
    req.newton_eps = j.value("newtonEps", 1e-13);
    req.classify_eps = j.value("classifyEps", 1e-10);
    req.root_merge_eps = j.value("rootMergeEps", 1e-10);
    req.include_variant_compatibility = j.value("includeVariantCompatibility", true);

    if (req.period_min < 1 || req.period_min > 10) {
        throw HttpError(400, Json{{"error", "invalid period"}, {"limit", "1..10"}}.dump());
    }
    const double re = j.value("re", j.value("real", 0.0));
    const double im = j.value("im", j.value("imag", 0.0));
    compute::SpecialPointResult point = compute::find_hyperbolic_center_near({re, im}, req.period_min, req);
    return Json{{"point", pointToJson(point)}}.dump();
}

std::string specialPointsAutoRoute(const std::filesystem::path& repoRoot, const std::string& body) {
    const Json j = parseJsonBody(body);
    const int k = j.value("k", 1);
    const int p = j.value("p", 1);
    const std::string pointType = j.value("pointType", std::string(k == 0 ? "hyperbolic" : "misiurewicz"));

    if (pointType == "misiurewicz" && k <= 0) throw std::runtime_error("misiurewicz requires k > 0");
    if (pointType == "hyperbolic"  && k != 0) throw std::runtime_error("hyperbolic requires k = 0");
    if (p < 1)                                 throw std::runtime_error("period p must be >= 1");

    const auto pts = compute::newton_sp::auto_solve(k, p);

    Json items;
    persistPoints(repoRoot, pts, "auto", pointType, items);

    Json resp = {
        {"mode", "auto"},
        {"k", k},
        {"p", p},
        {"count", pts.size()},
        {"points", items},
    };
    return resp.dump();
}

std::string specialPointsSeedRoute(const std::filesystem::path& repoRoot, const std::string& body) {
    const Json j = parseJsonBody(body);
    const int k = j.value("k", 1);
    const int p = j.value("p", 1);
    const double sr = j.value("re", 0.0);
    const double si = j.value("im", 0.0);

    if (p < 1) throw std::runtime_error("period p must be >= 1");

    const auto pts = compute::newton_sp::seed_solve(k, p, std::complex<double>(sr, si));

    Json items;
    persistPoints(repoRoot, pts, "seed", "newton", items);

    Json resp = {
        {"mode", "seed"},
        {"k", k},
        {"p", p},
        {"converged", !pts.empty()},
        {"points", items},
    };
    return resp.dump();
}

std::string specialPointsListRoute(const std::filesystem::path& repoRoot, const std::string& query) {
    const std::string family = urlDecode(getQueryParam(query, "family"));
    int k = -1, p = -1;
    const std::string kRaw = getQueryParam(query, "k");
    const std::string pRaw = getQueryParam(query, "p");
    if (!kRaw.empty()) k = std::stoi(kRaw);
    if (!pRaw.empty()) p = std::stoi(pRaw);

    Db db = openDb(repoRoot);
    const auto rows = db.listSpecialPoints(family, k, p);

    Json items = Json::array();
    for (const auto& r : rows) {
        items.push_back({
            {"id",         r.id},
            {"family",     r.family},
            {"pointType",  r.pointType},
            {"k",          r.k},
            {"p",          r.p},
            {"real",       r.re},
            {"imag",       r.im},
            {"sourceMode", r.sourceMode},
            {"createdAt",  r.createdAt},
        });
    }
    Json resp = {{"items", items}};
    return resp.dump();
}

} // namespace fsd
