// POST /api/benchmark -- calibrate the production map-field engine paths.

#include "routes.hpp"
#include "routes_common.hpp"
#include "resource_manager.hpp"

#include "../compute/engine_select.hpp"
#include "../compute/map_kernel.hpp"
#include "../compute/tile_scheduler.hpp"

#if defined(HAS_CUDA_KERNEL)
#  include "../compute/cuda/map_kernel.cuh"
#  define USE_CUDA 1
#else
#  define USE_CUDA 0
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fsd {

namespace {

constexpr int kMinBenchmarkDimension = 64;
constexpr int kMaxBenchmarkDimension = 2048;
constexpr long long kMaxBenchmarkPixels = 2048LL * 2048LL;
constexpr double kMaxBenchmarkWorkUnits = 8.0e9;
constexpr int kMinBenchmarkIterations = 1;
constexpr int kMaxBenchmarkIterations = 100000;
constexpr int kMaxBenchmarkSamples = 7;
constexpr int kMaxBenchmarkWarmups = 3;

[[noreturn]] void invalidBenchmarkParameter(
    const std::string& field,
    const std::string& limit
) {
    throw HttpError(400, Json{
        {"error", "invalid benchmark parameter"},
        {"field", field},
        {"limit", limit},
    }.dump());
}

int integerParameter(
    const Json& body,
    const char* key,
    int fallback,
    int minimum,
    int maximum
) {
    const auto it = body.find(key);
    if (it == body.end()) return fallback;
    if (!it->is_number()) invalidBenchmarkParameter(key, std::to_string(minimum) + ".." + std::to_string(maximum));
    const double value = it->get<double>();
    if (!std::isfinite(value) || std::floor(value) != value ||
        value < static_cast<double>(minimum) || value > static_cast<double>(maximum)) {
        invalidBenchmarkParameter(key, std::to_string(minimum) + ".." + std::to_string(maximum));
    }
    return static_cast<int>(value);
}

double finiteParameter(const Json& body, const char* key, double fallback) {
    const auto it = body.find(key);
    if (it == body.end()) return fallback;
    if (!it->is_number()) invalidBenchmarkParameter(key, "finite number");
    const double value = it->get<double>();
    if (!std::isfinite(value)) invalidBenchmarkParameter(key, "finite number");
    return value;
}

bool booleanParameter(const Json& body, const char* key, bool fallback) {
    const auto it = body.find(key);
    if (it == body.end()) return fallback;
    if (!it->is_boolean()) invalidBenchmarkParameter(key, "boolean");
    return it->get<bool>();
}

int warmupParameter(const Json& body) {
    const auto it = body.find("warmup");
    if (it == body.end()) return 1;
    if (it->is_boolean()) return it->get<bool>() ? 1 : 0;
    return integerParameter(body, "warmup", 1, 0, kMaxBenchmarkWarmups);
}

std::string workloadParameter(const Json& body, long long pixels) {
    const auto it = body.find("workload");
    if (it == body.end()) return pixels < 900000LL ? "interactive" : "batch";
    if (!it->is_string()) invalidBenchmarkParameter("workload", "1..64 characters: [A-Za-z0-9_.-]");
    const std::string value = it->get<std::string>();
    if (value.empty() || value.size() > 64 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
        })) {
        invalidBenchmarkParameter("workload", "1..64 characters: [A-Za-z0-9_.-]");
    }
    return value;
}

void setBenchmarkProgress(
    JobRunner& runner,
    const std::string& runId,
    const std::string& stage,
    int current,
    int total,
    Json details = Json::object(),
    bool cancelable = false,
    bool kernelReported = false
) {
    const double percent = total > 0
        ? 100.0 * static_cast<double>(current) / static_cast<double>(total)
        : 0.0;
    runner.setProgress(runId, Json{
        {"taskType", "benchmark"},
        {"stage", stage},
        {"current", current},
        {"total", total},
        {"percent", percent},
        {"kernelReported", kernelReported},
        {"elapsedMs", runner.runElapsedMs(runId)},
        {"cancelable", cancelable},
        {"resourceLocks", Json::array({"benchmark", "cpu_heavy", "cuda_heavy"})},
        {"details", std::move(details)},
    }.dump());
}

double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    if ((values.size() & 1U) != 0U) return values[middle];
    return 0.5 * (values[middle - 1] + values[middle]);
}

bool requestedPathWasUsed(
    const std::string& requestedEngine,
    const std::string& requestedScalar,
    const std::string& actualEngine,
    const std::string& actualScalar
) {
    return requestedEngine == actualEngine && requestedScalar == actualScalar;
}

struct RenderMeasurement {
    std::string actualEngine;
    std::string actualScalar;
    double elapsedMs = 0.0;
};

struct BenchResult {
    std::string family = "map_field";
    std::string workload;
    std::string engine;
    std::string scalar;
    std::string actualEngine;
    std::string actualScalar;
    std::string error;
    double workUnits = 0.0;
    double elapsedMs = 0.0;
    double mpixPerSec = 0.0;
    bool available = false;
    int warmupCount = 0;
    int sampleCount = 0;
    std::vector<double> sampleElapsedMs;
};

Json resultJson(const BenchResult& result, int requestedSamples, int requestedWarmups) {
    return Json{
        {"family", result.family},
        {"workload", result.workload},
        {"workUnits", result.workUnits},
        {"engine", result.engine},
        {"scalar", result.scalar},
        {"requestedEngine", result.engine},
        {"requestedScalar", result.scalar},
        {"actualEngine", result.actualEngine},
        {"actualScalar", result.actualScalar},
        {"available", result.available},
        {"elapsedMs", result.elapsedMs},
        {"mpixPerSec", result.mpixPerSec},
        {"warmupCount", result.warmupCount},
        {"requestedWarmupCount", requestedWarmups},
        {"sampleCount", result.sampleCount},
        {"requestedSampleCount", requestedSamples},
        {"sampleElapsedMs", result.sampleElapsedMs},
        {"error", result.error},
    };
}

compute::BenchmarkEntry cacheEntry(const BenchResult& result) {
    compute::BenchmarkEntry entry;
    entry.engine = result.engine;
    entry.scalar = result.scalar;
    entry.mpix_per_sec = result.mpixPerSec;
    entry.available = result.available;
    entry.family = result.family;
    entry.workload = result.workload;
    entry.work_units = result.workUnits;
    entry.elapsed_ms = result.elapsedMs;
    entry.sample_count = result.sampleCount;
    return entry;
}

} // namespace

std::string benchmarkRoute(JobRunner& runner, const std::string& body) {
    Json jbody;
    try {
        jbody = body.empty() ? Json::object() : Json::parse(body);
    } catch (const std::exception&) {
        invalidBenchmarkParameter("body", "valid JSON object");
    }
    if (!jbody.is_object()) invalidBenchmarkParameter("body", "JSON object");

    const double centerRe = finiteParameter(jbody, "centerRe", -0.75);
    const double centerIm = finiteParameter(jbody, "centerIm", 0.0);
    const double scale = finiteParameter(jbody, "scale", 1.5);
    if (!(scale > 0.0)) invalidBenchmarkParameter("scale", "finite number > 0");

    const int width = integerParameter(
        jbody, "width", 512, kMinBenchmarkDimension, kMaxBenchmarkDimension);
    const int height = integerParameter(
        jbody, "height", 512, kMinBenchmarkDimension, kMaxBenchmarkDimension);
    const int iterations = integerParameter(
        jbody, "iterations", 2000, kMinBenchmarkIterations, kMaxBenchmarkIterations);
    const int requestedSamples = integerParameter(
        jbody, "samples", 3, 1, kMaxBenchmarkSamples);
    const int requestedWarmups = warmupParameter(jbody);
    const bool replaceCache = booleanParameter(jbody, "replaceCache", true);

    const long long pixels = static_cast<long long>(width) * static_cast<long long>(height);
    if (pixels > kMaxBenchmarkPixels) {
        invalidBenchmarkParameter("width,height", "at most 4194304 pixels");
    }
    const std::string workload = workloadParameter(jbody, pixels);
    const double workUnits = static_cast<double>(pixels) * static_cast<double>(iterations);
    if (workUnits > kMaxBenchmarkWorkUnits) {
        invalidBenchmarkParameter(
            "width,height,iterations",
            "width * height * iterations must not exceed 8000000000");
    }

    auto run = runner.createRun("benchmark", body);
    const bool background = jbody.value("background", false);
    ResourceManager::Lease rawLease;
    std::string conflictLock, activeRunId;
    if (!resourceManager().tryAcquire(
            run.id, "benchmark", {"benchmark", "cpu_heavy", "cuda_heavy"},
            rawLease, conflictLock, activeRunId)) {
        runner.setStatus(run.id, "failed");
        throw HttpError(409, Json{
            {"error", "benchmark already running"},
            {"activeRunId", activeRunId},
            {"taskType", "benchmark"},
            {"resourceLock", conflictLock},
        }.dump());
    }
    auto lease = std::make_shared<ResourceManager::Lease>(std::move(rawLease));
    const auto cancelToken = runner.cancelToken(run.id);
    runner.setCancelable(run.id, true);
    setBenchmarkProgress(runner, run.id, "queued", 0, 0, Json::object(), true);

    auto execute = [=, &runner]() mutable -> Json {
        (void)lease;
        runner.setStatus(run.id, "running");
        int completed = 0;
        int total = 0;
        try {
        struct Candidate {
            std::string engine;
            std::string scalar;
        };
        std::vector<Candidate> candidates = {
            {"openmp", "fp32"},
            {"openmp", "fp64"},
            {"openmp", "fx64"},
        };

        const auto capabilities = compute::runtime_capabilities();
        const bool avx2Ok = capabilities.avx2_compiled && capabilities.avx2_runtime && capabilities.fma_runtime;
        const bool avx512Ok = capabilities.avx512_compiled && capabilities.avx512_runtime;
#if USE_CUDA
        const bool cudaOk = capabilities.cuda_compiled && capabilities.cuda_runtime && fsd_cuda::cuda_available();
#else
        const bool cudaOk = false;
#endif
        if (avx2Ok) {
            candidates.push_back({"avx2", "fp32"});
            candidates.push_back({"avx2", "fp64"});
        }
        if (avx512Ok) {
            candidates.push_back({"avx512", "fp32"});
            candidates.push_back({"avx512", "fp64"});
        }
        if (cudaOk) {
            candidates.push_back({"cuda", "fp32"});
            candidates.push_back({"cuda", "fp64"});
            candidates.push_back({"cuda", "fx64"});
            candidates.push_back({"hybrid", "fp64"});
        }
        total = static_cast<int>(candidates.size());
        setBenchmarkProgress(runner, run.id, "running", 0, total, Json::object(), true);

        std::vector<BenchResult> results;
        results.reserve(candidates.size());

        auto renderOnce = [&](const Candidate& candidate) -> RenderMeasurement {
            compute::MapParams params;
            params.center_re = centerRe;
            params.center_im = centerIm;
            params.scale = scale;
            params.width = width;
            params.height = height;
            params.iterations = iterations;
            params.engine = candidate.engine;
            params.scalar_type = candidate.scalar;

            compute::FieldOutput field;
            RenderMeasurement measurement;
            const auto started = std::chrono::steady_clock::now();
            if (candidate.engine == "hybrid") {
                const auto stats = compute::render_map_field_hybrid(params, field);
                measurement.actualEngine = stats.engine_used;
                measurement.actualScalar = stats.scalar_used;
            } else {
                const auto stats = compute::render_map_field(params, field);
                measurement.actualEngine = stats.engine_used;
                measurement.actualScalar = stats.scalar_used;
            }
            const auto finished = std::chrono::steady_clock::now();
            measurement.elapsedMs = std::chrono::duration<double, std::milli>(finished - started).count();
            return measurement;
        };

        for (const auto& candidate : candidates) {
            if (cancelToken->load(std::memory_order_relaxed)) {
                throw std::runtime_error("cancelled");
            }
            setBenchmarkProgress(runner, run.id, "running", completed, total, Json{
                {"family", "map_field"},
                {"workload", workload},
                {"requestedEngine", candidate.engine},
                {"requestedScalar", candidate.scalar},
            }, true);

            BenchResult result;
            result.workload = workload;
            result.workUnits = workUnits;
            result.engine = candidate.engine;
            result.scalar = candidate.scalar;

            auto acceptMeasurement = [&](const RenderMeasurement& measurement) {
                result.actualEngine = measurement.actualEngine;
                result.actualScalar = measurement.actualScalar;
                if (!requestedPathWasUsed(
                        result.engine, result.scalar,
                        measurement.actualEngine, measurement.actualScalar)) {
                    result.error = "requested path fell back to " +
                        (measurement.actualEngine.empty() ? std::string("unknown") : measurement.actualEngine) + "/" +
                        (measurement.actualScalar.empty() ? std::string("unknown") : measurement.actualScalar);
                    return false;
                }
                return true;
            };

            try {
                bool pathMatches = true;
                for (int warmup = 0; warmup < requestedWarmups; ++warmup) {
                    if (cancelToken->load(std::memory_order_relaxed)) {
                        throw std::runtime_error("cancelled");
                    }
                    const RenderMeasurement measurement = renderOnce(candidate);
                    ++result.warmupCount;
                    if (!acceptMeasurement(measurement)) {
                        pathMatches = false;
                        break;
                    }
                }

                for (int sample = 0; pathMatches && sample < requestedSamples; ++sample) {
                    if (cancelToken->load(std::memory_order_relaxed)) {
                        throw std::runtime_error("cancelled");
                    }
                    const RenderMeasurement measurement = renderOnce(candidate);
                    if (!acceptMeasurement(measurement)) {
                        pathMatches = false;
                        break;
                    }
                    result.sampleElapsedMs.push_back(measurement.elapsedMs);
                }

                result.sampleCount = static_cast<int>(result.sampleElapsedMs.size());
                result.elapsedMs = median(result.sampleElapsedMs);
                if (result.elapsedMs > 0.0) {
                    result.mpixPerSec =
                        (static_cast<double>(pixels) / 1e6) / (result.elapsedMs / 1000.0);
                }
                result.available = pathMatches && result.sampleCount == requestedSamples;
                if (!result.available && result.error.empty()) {
                    result.error = "benchmark did not collect every requested sample";
                }
            } catch (const std::exception& ex) {
                result.error = ex.what();
                result.available = false;
                result.sampleCount = static_cast<int>(result.sampleElapsedMs.size());
                result.elapsedMs = median(result.sampleElapsedMs);
            } catch (...) {
                result.error = "unknown benchmark error";
                result.available = false;
                result.sampleCount = static_cast<int>(result.sampleElapsedMs.size());
                result.elapsedMs = median(result.sampleElapsedMs);
            }

            results.push_back(std::move(result));
            ++completed;
            const BenchResult& saved = results.back();
            setBenchmarkProgress(runner, run.id, "running", completed, total, Json{
                {"family", saved.family},
                {"workload", saved.workload},
                {"requestedEngine", saved.engine},
                {"requestedScalar", saved.scalar},
                {"actualEngine", saved.actualEngine},
                {"actualScalar", saved.actualScalar},
                {"available", saved.available},
            }, true);
        }

        Json jsonResults = Json::array();
        std::vector<compute::BenchmarkEntry> cacheEntries;
        cacheEntries.reserve(results.size());
        for (const auto& result : results) {
            jsonResults.push_back(resultJson(result, requestedSamples, requestedWarmups));
            cacheEntries.push_back(cacheEntry(result));
        }

        Json response = {
            {"runId", run.id},
            {"status", "completed"},
            {"family", "map_field"},
            {"workload", workload},
            {"workUnits", workUnits},
            {"centerRe", centerRe},
            {"centerIm", centerIm},
            {"scale", scale},
            {"width", width},
            {"height", height},
            {"iterations", iterations},
            {"warmup", requestedWarmups},
            {"samples", requestedSamples},
            {"replaceCache", replaceCache},
            {"results", jsonResults},
            {"artifact", {
                {"name", "benchmark.json"},
                {"kind", "report"},
            }},
        };

        const std::filesystem::path reportPath =
            std::filesystem::path(run.outputDir) / "benchmark.json";
        atomicWriteText(reportPath, response.dump(2));
        runner.addArtifact(run.id, Artifact{"benchmark", reportPath.string(), "report"});

        if (replaceCache) {
            compute::update_benchmark_cache(cacheEntries);
        } else {
            compute::merge_benchmark_cache(cacheEntries);
        }

        setBenchmarkProgress(runner, run.id, "completed", total, total, Json{
            {"family", "map_field"},
            {"workload", workload},
            {"workUnits", workUnits},
            {"resultCount", results.size()},
            {"artifact", "benchmark.json"},
            {"hardwareExecutions", jsonResults},
        }, false, true);
        runner.setCancelable(run.id, false);
        runner.setStatus(run.id, "completed");
        return response;
        } catch (const std::exception& ex) {
        try {
            const bool cancelled = cancelToken->load(std::memory_order_relaxed) ||
                std::string(ex.what()) == "cancelled";
            setBenchmarkProgress(runner, run.id, cancelled ? "cancelled" : "failed",
                                 completed, total, Json{{"error", ex.what()}});
            runner.setCancelable(run.id, false);
            runner.setStatus(run.id, cancelled ? "cancelled" : "failed");
        } catch (...) {}
        throw;
        } catch (...) {
        try {
            setBenchmarkProgress(runner, run.id, "failed", completed, total, Json{{"error", "unknown benchmark error"}});
            runner.setCancelable(run.id, false);
            runner.setStatus(run.id, "failed");
        } catch (...) {}
        throw;
        }
    };

    if (background) {
        auto backgroundToken = runner.backgroundTaskToken();
        std::thread([execute, backgroundToken]() mutable {
            (void)backgroundToken;
            try {
                (void)execute();
            } catch (...) {}
        }).detach();
        return Json{{"runId", run.id}, {"status", "queued"}}.dump();
    }

    return execute().dump();
}

} // namespace fsd
