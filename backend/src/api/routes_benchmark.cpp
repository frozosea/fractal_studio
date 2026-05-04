// routes_benchmark.cpp
//
// POST /api/benchmark — runs a reference viewport through all engine×scalar
// combinations and returns a throughput table.
//
// Reference viewport: center=(-0.75, 0), scale=1.5, 1024×1024, iter=10000.
// Each engine runs once (no warmup for simplicity). Results are in Mpix/s.

#include "routes.hpp"
#include "routes_common.hpp"
#include "resource_manager.hpp"

#include "../compute/map_kernel.hpp"
#include "../compute/map_kernel_avx2.hpp"
#include "../compute/map_kernel_avx512.hpp"
#include "../compute/engine_select.hpp"
#include "../compute/tile_scheduler.hpp"

#if defined(HAS_CUDA_KERNEL)
#  include "../compute/cuda/map_kernel.cuh"
#  define USE_CUDA 1
#else
#  define USE_CUDA 0
#endif

#include <opencv2/core.hpp>
#include <chrono>
#include <vector>

namespace fsd {

namespace {

void setBenchmarkProgress(JobRunner& runner, const std::string& runId, const std::string& stage, int current, int total) {
    const double percent = total > 0 ? (100.0 * static_cast<double>(current) / static_cast<double>(total)) : 0.0;
    runner.setProgress(runId, Json{
        {"taskType", "benchmark"},
        {"stage", stage},
        {"current", current},
        {"total", total},
        {"percent", percent},
        {"elapsedMs", runner.runElapsedMs(runId)},
        {"cancelable", false},
        {"resourceLocks", Json::array({"benchmark", "cpu_heavy", "cuda_heavy"})},
        {"details", Json::object()},
    }.dump());
}

} // namespace

std::string benchmarkRoute(JobRunner& runner, const std::string& body) {
    const Json jbody = body.empty() ? Json::object() : parseJsonBody(body);

    const double cRe   = jbody.value("centerRe",   -0.75);
    const double cIm   = jbody.value("centerIm",    0.0);
    const double scale = jbody.value("scale",       1.5);
    const int W        = jbody.value("width",       512);
    const int H        = jbody.value("height",      512);
    const int iters    = jbody.value("iterations",  2000);

    auto run = runner.createRun("benchmark", body);
    ResourceManager::Lease lease;
    std::string conflictLock, activeRunId;
    if (!resourceManager().tryAcquire(run.id, "benchmark", {"benchmark", "cpu_heavy", "cuda_heavy"}, lease, conflictLock, activeRunId)) {
        runner.setStatus(run.id, "failed");
        throw HttpError(409, Json{
            {"error", "benchmark already running"},
            {"activeRunId", activeRunId},
            {"taskType", "benchmark"},
            {"resourceLock", conflictLock},
        }.dump());
    }
    (void)lease;
    runner.setStatus(run.id, "running");
    runner.setCancelable(run.id, false);

    struct BenchResult {
        std::string engine;
        std::string scalar;
        double elapsed_ms;
        double mpix_per_sec;
        bool  available;
    };

    std::vector<BenchResult> results;

    auto run_bench = [&](const std::string& engine, const std::string& scalar) -> BenchResult {
        BenchResult r;
        r.engine   = engine;
        r.scalar   = scalar;
        r.available = false;
        r.elapsed_ms = 0.0;
        r.mpix_per_sec = 0.0;

        compute::MapParams p;
        p.center_re   = cRe;
        p.center_im   = cIm;
        p.scale       = scale;
        p.width       = W;
        p.height      = H;
        p.iterations  = iters;
        p.engine      = engine;
        p.scalar_type = scalar;

        cv::Mat out;
        try {
            const auto t0 = std::chrono::steady_clock::now();
            if (engine == "hybrid") {
                auto stats = compute::render_map_hybrid(p, out);
                (void)stats;
            } else {
                auto stats = compute::render_map(p, out);
                (void)stats;
            }
            const auto t1 = std::chrono::steady_clock::now();
            r.elapsed_ms = std::chrono::duration<double,std::milli>(t1 - t0).count();
            r.mpix_per_sec = (static_cast<double>(W) * H / 1e6) / (r.elapsed_ms / 1000.0);
            r.available = true;
        } catch (...) {
            r.available = false;
        }
        return r;
    };

    const bool avx2_ok = compute::avx2_available() && compute::fma_available();
    const bool avx512_ok = compute::avx512_available();
#if USE_CUDA
    const bool cuda_ok = fsd_cuda::cuda_available();
#else
    const bool cuda_ok = false;
#endif
    const int total = 3
        + (avx2_ok ? 2 : 0)
        + (avx512_ok ? 2 : 0)
        + (cuda_ok ? 5 : 0);

    setBenchmarkProgress(runner, run.id, "running", 0, total);

    // OpenMP fp32 / fp64 / fx64
    results.push_back(run_bench("openmp", "fp32"));
    setBenchmarkProgress(runner, run.id, "running", static_cast<int>(results.size()), total);
    results.push_back(run_bench("openmp", "fp64"));
    setBenchmarkProgress(runner, run.id, "running", static_cast<int>(results.size()), total);
    results.push_back(run_bench("openmp", "fx64"));
    setBenchmarkProgress(runner, run.id, "running", static_cast<int>(results.size()), total);

    // AVX2/FMA (mainstream CPU SIMD path)
    if (avx2_ok) {
        results.push_back(run_bench("avx2", "fp32"));
        setBenchmarkProgress(runner, run.id, "running", static_cast<int>(results.size()), total);
        results.push_back(run_bench("avx2", "fp64"));
        setBenchmarkProgress(runner, run.id, "running", static_cast<int>(results.size()), total);
    }

    // AVX-512 (only if available)
    if (avx512_ok) {
        results.push_back(run_bench("avx512", "fp32"));
        setBenchmarkProgress(runner, run.id, "running", static_cast<int>(results.size()), total);
        results.push_back(run_bench("avx512", "fp64"));
        setBenchmarkProgress(runner, run.id, "running", static_cast<int>(results.size()), total);
    }

    // CUDA (only if available)
    if (cuda_ok) {
        // CUDA path via render_map — uses CUDA internally when engine="cuda"
        results.push_back(run_bench("cuda", "fp32"));
        setBenchmarkProgress(runner, run.id, "running", static_cast<int>(results.size()), total);
        results.push_back(run_bench("cuda", "fp64"));
        setBenchmarkProgress(runner, run.id, "running", static_cast<int>(results.size()), total);
        results.push_back(run_bench("cuda", "fx64"));
        setBenchmarkProgress(runner, run.id, "running", static_cast<int>(results.size()), total);
        results.push_back(run_bench("hybrid", "fp64"));
        setBenchmarkProgress(runner, run.id, "running", static_cast<int>(results.size()), total);
        results.push_back(run_bench("hybrid", "fx64"));
        setBenchmarkProgress(runner, run.id, "running", static_cast<int>(results.size()), total);
    }

    Json jresults = Json::array();
    std::vector<compute::BenchmarkEntry> cache_entries;
    for (const auto& r : results) {
        jresults.push_back({
            {"engine",     r.engine},
            {"scalar",     r.scalar},
            {"available",  r.available},
            {"elapsedMs",  r.elapsed_ms},
            {"mpixPerSec", r.mpix_per_sec},
        });
        cache_entries.push_back({r.engine, r.scalar, r.mpix_per_sec, r.available});
    }
    compute::update_benchmark_cache(cache_entries);

    Json resp = {
        {"runId",      run.id},
        {"status",     "completed"},
        {"width",      W},
        {"height",     H},
        {"iterations", iters},
        {"results",    jresults},
    };
    setBenchmarkProgress(runner, run.id, "completed", total, total);
    runner.setStatus(run.id, "completed");
    return resp.dump();
}

} // namespace fsd
