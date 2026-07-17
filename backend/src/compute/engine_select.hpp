// compute/engine_select.hpp
//
// Runtime capability and benchmark-aware engine selection.

#pragma once

#include "map_kernel.hpp"

#include <optional>
#include <string>
#include <vector>

namespace fsd::compute {

struct RuntimeCapabilities {
    bool openmp_compiled = false;
    bool openmp_runtime = false;
    bool avx2_compiled = false;
    bool avx2_runtime = false;
    bool fma_runtime = false;
    bool bmi2_runtime = false;
    bool avx512_compiled = false;
    bool avx512_runtime = false;
    bool avx512ifma_runtime = false;
    bool cuda_compiled = false;
    bool cuda_runtime = false;
    bool cuda_low_end = false;
    int cuda_device_count = 0;
    int cuda_compute_major = 0;
    int cuda_compute_minor = 0;
    unsigned long long cuda_total_vram = 0;
    unsigned long long cuda_free_vram = 0;
    std::string cuda_name;
    int logical_cores = 1;
    int physical_cores = 1;
};

struct BenchmarkEntry {
    std::string engine;
    std::string scalar;
    double mpix_per_sec = 0.0;
    bool available = false;
    // Scheduling metadata. Keep these fields after the legacy four above so
    // existing aggregate initializers remain source-compatible.
    std::string family = "map_field";
    std::string workload;
    double work_units = 0.0;
    double elapsed_ms = 0.0;
    int sample_count = 1;
};

RuntimeCapabilities runtime_capabilities();

// `update_benchmark_cache` is the legacy replace operation. New callers should
// use the explicit replace/merge names so a per-family refresh cannot
// accidentally discard calibration for every other scheduler family.
void update_benchmark_cache(const std::vector<BenchmarkEntry>& entries);
void replace_benchmark_cache(const std::vector<BenchmarkEntry>& entries);
void merge_benchmark_cache(const std::vector<BenchmarkEntry>& entries);
void clear_benchmark_cache();
bool has_benchmark_cache();
std::vector<BenchmarkEntry> benchmark_cache_snapshot();

// Pure profile helpers. They deliberately do not inspect runtime hardware, so
// capability/correctness filtering remains the caller's responsibility and
// selector behaviour can be tested with synthetic profiles on any machine.
std::optional<double> predict_profiled_elapsed_ms(
    const std::vector<BenchmarkEntry>& entries,
    const std::string& family,
    const std::string& scalar,
    const std::string& engine,
    double work_units);
std::string select_profiled_engine(
    const std::vector<BenchmarkEntry>& entries,
    const std::string& family,
    const std::string& scalar,
    double work_units,
    const std::vector<std::string>& candidates,
    const std::string& fallback_engine = {});

bool map_engine_supported(const MapParams& p, const std::string& engine, bool fx);
bool map_work_is_large(const MapParams& p);
std::string select_map_engine(const MapParams& p, bool fx, const std::string& purpose = "map");

} // namespace fsd::compute
