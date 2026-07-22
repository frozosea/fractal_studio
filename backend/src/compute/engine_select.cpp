// compute/engine_select.cpp

#include "engine_select.hpp"

#include "cpu_features.hpp"
#include "map_kernel_avx2.hpp"
#include "map_kernel_avx512.hpp"
#include "parallel.hpp"

#if defined(HAS_CUDA_KERNEL)
#  include "cuda/map_kernel.cuh"
#  define USE_CUDA 1
#else
#  define USE_CUDA 0
#endif

#ifdef _OPENMP
#  include <omp.h>
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

namespace fsd::compute {
namespace {

std::mutex g_bench_mu;
std::vector<BenchmarkEntry> g_bench_entries;

int physical_core_guess(int logical) {
    return std::max(1, logical / 2);
}

bool is_cuda_metric_supported(Metric metric) {
    return static_cast<int>(metric) >= 0 && static_cast<int>(metric) < 4;
}

bool is_vector_metric_supported(Metric metric) {
    return static_cast<int>(metric) >= 0 && static_cast<int>(metric) < 4;
}

bool is_low_end_cuda_device(const RuntimeCapabilities& c) {
    const bool pascal_or_older = c.cuda_compute_major > 0 && c.cuda_compute_major <= 6;
    const bool small_vram = c.cuda_total_vram > 0 &&
        c.cuda_total_vram <= 4ULL * 1024ULL * 1024ULL * 1024ULL;
    return pascal_or_older || small_vram;
}

bool is_small_interactive_map(const MapParams& p) {
    const long long pixels = static_cast<long long>(p.width) * static_cast<long long>(p.height);
    const long long work = pixels * static_cast<long long>(std::max(1, p.iterations));
    return pixels < 900000LL || work < 600000000LL;
}

bool same_benchmark_slot(const BenchmarkEntry& a, const BenchmarkEntry& b) {
    return a.family == b.family &&
           a.workload == b.workload &&
           a.engine == b.engine &&
           a.scalar == b.scalar &&
           a.work_units == b.work_units;
}

struct CostPoint {
    double work_units = 0.0;
    double elapsed_ms = 0.0;
    int sample_count = 1;
};

double interpolate_cost(const CostPoint& a, const CostPoint& b, double target) {
    const double span = b.work_units - a.work_units;
    if (!(span > 0.0)) return std::max(0.001, a.elapsed_ms);
    // Timings can contain small inversions from clock/thermal noise. A negative
    // slope must not predict that a larger render completes sooner.
    const double slope = std::max(0.0, (b.elapsed_ms - a.elapsed_ms) / span);
    const double predicted = a.elapsed_ms + (target - a.work_units) * slope;
    return std::max(0.001, predicted);
}

std::string canonical_map_profile_scalar(const MapParams& p, bool fx) {
    if (fx) return "fx64";
    const std::string scalar = map_effective_scalar_type(p);
    if (scalar == "fp32") return "fp32";
    if (map_scalar_type_is_fp80(scalar)) return "fp80";
    if (map_scalar_type_is_fp128(scalar)) return "fp128";
    // An unrepresentable fixed-point request falls through to the fp64 map
    // kernel, so its performance profile must be fp64 as well.
    return "fp64";
}

double map_work_units(const MapParams& p) {
    return static_cast<double>(std::max(1, p.width)) *
           static_cast<double>(std::max(1, p.height)) *
           static_cast<double>(std::max(1, p.iterations));
}

std::vector<std::string> supported_in_order(
    const MapParams& p,
    bool fx,
    const std::vector<std::string>& order)
{
    std::vector<std::string> out;
    out.reserve(order.size());
    for (const std::string& engine : order) {
        if (std::find(out.begin(), out.end(), engine) != out.end()) continue;
        if (map_engine_supported(p, engine, fx)) out.push_back(engine);
    }
    return out;
}

} // namespace

RuntimeCapabilities runtime_capabilities() {
    RuntimeCapabilities c;
#ifdef _OPENMP
    c.openmp_compiled = true;
    c.openmp_runtime = omp_get_max_threads() > 0;
#else
    c.openmp_compiled = false;
    c.openmp_runtime = true;
#endif

#if defined(HAS_AVX2_KERNEL)
    c.avx2_compiled = true;
#endif
    c.avx2_runtime = avx2_available();
    c.fma_runtime = fma_available();
    c.bmi2_runtime = bmi2_available();

#if defined(HAS_AVX512_KERNEL)
    c.avx512_compiled = true;
#endif
    c.avx512_runtime = avx512_available();
    c.avx512ifma_runtime = avx512ifma_available();

#if USE_CUDA
    c.cuda_compiled = true;
    c.cuda_runtime = fsd_cuda::cuda_available();
    const auto cuda = fsd_cuda::cuda_device_info();
    c.cuda_device_count = cuda.device_count;
    c.cuda_compute_major = cuda.major;
    c.cuda_compute_minor = cuda.minor;
    c.cuda_total_vram = static_cast<unsigned long long>(cuda.total_global_mem);
    c.cuda_free_vram = static_cast<unsigned long long>(cuda.free_global_mem);
    c.cuda_name = cuda.name;
    c.cuda_low_end = is_low_end_cuda_device(c);
#endif

    c.logical_cores = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    c.physical_cores = physical_core_guess(c.logical_cores);
    return c;
}

void update_benchmark_cache(const std::vector<BenchmarkEntry>& entries) {
    replace_benchmark_cache(entries);
}

void replace_benchmark_cache(const std::vector<BenchmarkEntry>& entries) {
    std::lock_guard<std::mutex> lk(g_bench_mu);
    g_bench_entries = entries;
}

void merge_benchmark_cache(const std::vector<BenchmarkEntry>& entries) {
    std::lock_guard<std::mutex> lk(g_bench_mu);
    for (const BenchmarkEntry& incoming : entries) {
        const auto it = std::find_if(
            g_bench_entries.begin(), g_bench_entries.end(),
            [&](const BenchmarkEntry& current) {
                return same_benchmark_slot(current, incoming);
            });
        if (it == g_bench_entries.end()) {
            g_bench_entries.push_back(incoming);
        } else {
            *it = incoming;
        }
    }
}

void clear_benchmark_cache() {
    std::lock_guard<std::mutex> lk(g_bench_mu);
    g_bench_entries.clear();
}

bool has_benchmark_cache() {
    std::lock_guard<std::mutex> lk(g_bench_mu);
    return std::any_of(g_bench_entries.begin(), g_bench_entries.end(), [](const BenchmarkEntry& entry) {
        return entry.available;
    });
}

std::vector<BenchmarkEntry> benchmark_cache_snapshot() {
    std::lock_guard<std::mutex> lk(g_bench_mu);
    return g_bench_entries;
}

std::optional<double> predict_profiled_elapsed_ms(
    const std::vector<BenchmarkEntry>& entries,
    const std::string& family,
    const std::string& scalar,
    const std::string& engine,
    double work_units)
{
    std::vector<CostPoint> raw_points;
    double legacy_speed = 0.0;
    for (const BenchmarkEntry& entry : entries) {
        if (!entry.available || entry.family != family ||
            entry.scalar != scalar || entry.engine != engine) {
            continue;
        }
        if (std::isfinite(entry.work_units) && entry.work_units > 0.0 &&
            std::isfinite(entry.elapsed_ms) && entry.elapsed_ms > 0.0) {
            raw_points.push_back({
                entry.work_units,
                entry.elapsed_ms,
                std::max(1, entry.sample_count),
            });
        }
        if (std::isfinite(entry.mpix_per_sec) && entry.mpix_per_sec > 0.0) {
            legacy_speed = std::max(legacy_speed, entry.mpix_per_sec);
        }
    }

    if (!raw_points.empty()) {
        std::sort(raw_points.begin(), raw_points.end(), [](const CostPoint& a, const CostPoint& b) {
            return a.work_units < b.work_units;
        });

        // Coalesce repeated calibration points (for example quick + manual
        // benchmark samples) using their declared sample counts.
        std::vector<CostPoint> points;
        points.reserve(raw_points.size());
        for (const CostPoint& point : raw_points) {
            if (points.empty() || points.back().work_units != point.work_units) {
                points.push_back(point);
                continue;
            }
            CostPoint& merged = points.back();
            const int total = merged.sample_count + point.sample_count;
            merged.elapsed_ms =
                (merged.elapsed_ms * static_cast<double>(merged.sample_count) +
                 point.elapsed_ms * static_cast<double>(point.sample_count)) /
                static_cast<double>(total);
            merged.sample_count = total;
        }

        const double target = std::max(0.0, work_units);
        if (points.size() == 1) {
            const CostPoint& only = points.front();
            if (!(target > 0.0)) return only.elapsed_ms;
            return std::max(0.001, only.elapsed_ms * target / only.work_units);
        }
        if (target <= points.front().work_units) {
            return interpolate_cost(points[0], points[1], target);
        }
        for (size_t i = 1; i < points.size(); ++i) {
            if (target <= points[i].work_units) {
                return interpolate_cost(points[i - 1], points[i], target);
            }
        }
        return interpolate_cost(points[points.size() - 2], points.back(), target);
    }

    // Backward compatibility for the original one-dimensional benchmark
    // table. Its absolute unit is not comparable with elapsed samples, but all
    // legacy entries in one selection share the same numerator, so it retains
    // the intended fastest-throughput ordering.
    if (legacy_speed > 0.0) {
        return std::max(1.0, work_units) / legacy_speed;
    }
    return std::nullopt;
}

std::string select_profiled_engine(
    const std::vector<BenchmarkEntry>& entries,
    const std::string& family,
    const std::string& scalar,
    double work_units,
    const std::vector<std::string>& candidates,
    const std::string& fallback_engine)
{
    std::string best;
    double best_ms = std::numeric_limits<double>::infinity();
    for (const std::string& engine : candidates) {
        const auto predicted = predict_profiled_elapsed_ms(
            entries, family, scalar, engine, work_units);
        if (!predicted.has_value() || !std::isfinite(*predicted)) continue;
        if (*predicted < best_ms) {
            best_ms = *predicted;
            best = engine;
        }
    }
    return best.empty() ? fallback_engine : best;
}

bool map_engine_supported(const MapParams& p, const std::string& engine, bool fx) {
    if (p.orbit_program || p.variant == Variant::Custom || p.custom_step_fn) return engine == "openmp";
    if (p.smooth) return engine == "openmp";
    if (variant_needs_scalar_fallback(p.variant)) return engine == "openmp";
    const std::string scalar = map_effective_scalar_type(p);
    if (map_scalar_type_is_fp80(scalar) || map_scalar_type_is_fp128(scalar)) return engine == "openmp";
    if (fx && p.metric == Metric::MinPairwiseDist) return engine == "openmp";

    const RuntimeCapabilities c = runtime_capabilities();
    if (engine == "openmp") return true;

    if (engine == "avx2") {
        if (!c.avx2_compiled || !c.avx2_runtime || !c.fma_runtime || !is_vector_metric_supported(p.metric)) return false;
        return !fx;
    }

    if (engine == "avx512") {
        if (!c.avx512_compiled || !c.avx512_runtime || !is_vector_metric_supported(p.metric)) return false;
        return !fx;
    }

    if (engine == "cuda") {
        return c.cuda_runtime && is_cuda_metric_supported(p.metric);
    }

    if (engine == "hybrid") {
        return c.cuda_runtime
            && is_cuda_metric_supported(p.metric)
            && map_work_is_large(p);
    }

    return false;
}

bool map_work_is_large(const MapParams& p) {
    return !is_small_interactive_map(p);
}

std::string select_map_engine(const MapParams& p, bool fx, const std::string& purpose) {
    if (p.engine != "auto") {
        // The raw-field hybrid implementation is fp64-only. Preserve the
        // historical safe downgrade for an explicit fp32 hybrid request.
        if (p.engine == "hybrid" && map_effective_scalar_type(p) == "fp32") {
            return map_engine_supported(p, "avx2", false) ? "avx2" : "openmp";
        }
        if (p.engine == "hybrid" && !map_work_is_large(p)) {
            if (map_engine_supported(p, "avx512", fx)) return "avx512";
            return map_engine_supported(p, "avx2", fx) ? "avx2" : "openmp";
        }
        return map_engine_supported(p, p.engine, fx) ? p.engine : "openmp";
    }

    if (p.orbit_program || p.variant == Variant::Custom || p.custom_step_fn || p.smooth || variant_needs_scalar_fallback(p.variant)) {
        return "openmp";
    }

    const std::string scalar = canonical_map_profile_scalar(p, fx);
    const bool large = map_work_is_large(p);
    std::vector<std::string> fallback_order;
    if (fx) {
        fallback_order = {"cuda", "openmp"};
    } else if (scalar == "fp32") {
        fallback_order = {"cuda", "avx512", "avx2", "openmp"};
    } else {
        // Preserve the former map-plan preference on an uncalibrated machine:
        // consumer GPU fp64 is only the fallback after the CPU SIMD tiers.
        fallback_order = {"avx512", "avx2", "cuda", "openmp"};
    }

    std::vector<std::string> candidates = supported_in_order(p, fx, fallback_order);
    if (!fx && scalar == "fp64" &&
        (large || purpose == "batch" || purpose == "volume") &&
        map_engine_supported(p, "hybrid", false)) {
        candidates.push_back("hybrid");
    }

    const std::string fallback = candidates.empty() ? "openmp" : candidates.front();
    // The current calibration suite represents the Escape raw-field pipeline.
    // Other metric paths keep their capability-filtered legacy ladder until
    // they receive a family/profile of their own.
    if (p.metric != Metric::Escape) return fallback;

    const std::vector<BenchmarkEntry> profile = benchmark_cache_snapshot();
    // A cache for another family/scalar is not evidence for this request.
    // Passing the static choice through the pure selector makes a missing key
    // fall back locally instead of mechanically selecting OpenMP merely because
    // the global cache is non-empty.
    return select_profiled_engine(
        profile, "map_field", scalar, map_work_units(p), candidates, fallback);
}

} // namespace fsd::compute
