// routes_modules.cpp — system check (openmp / cuda presence).
//
// Previously also dispatched legacy module runs — that surface is deleted.
// All native compute is exposed via dedicated endpoints (routes_map.cpp,
// routes_points.cpp, routes_hs.cpp, routes_ln.cpp, routes_video.cpp).

#include "routes.hpp"
#include "routes_common.hpp"
#include "system_checks.hpp"

#include "../compute/engine_select.hpp"
#include "../compute/parallel.hpp"

#include <algorithm>
#include <set>

namespace fsd {

std::string systemCheckRoute() {
    Json j = {
        {"openmp", checkOpenMP()},
        {"cuda",   checkCudaRuntime()},
    };
    return j.dump();
}

std::string systemCapabilitiesRoute() {
    const auto caps = compute::runtime_capabilities();
    Json bench = Json::array();
    Json benchmarkFamilies = Json::array();
    Json benchmarkWorkloads = Json::array();
    std::set<std::string> seenFamilies;
    std::set<std::string> seenWorkloads;
    int benchmarkSamples = 0;
    int availableProfiles = 0;
    const auto benchmarkEntries = compute::benchmark_cache_snapshot();
    for (const auto& e : benchmarkEntries) {
        bench.push_back({
            {"engine", e.engine},
            {"scalar", e.scalar},
            {"available", e.available},
            {"mpixPerSec", e.mpix_per_sec},
            {"family", e.family},
            {"workload", e.workload},
            {"workUnits", e.work_units},
            {"elapsedMs", e.elapsed_ms},
            {"sampleCount", e.sample_count},
        });
        benchmarkSamples += std::max(0, e.sample_count);
        if (e.available) ++availableProfiles;
        if (!e.family.empty() && seenFamilies.insert(e.family).second) {
            benchmarkFamilies.push_back(e.family);
        }
        if (!e.workload.empty() && seenWorkloads.insert(e.workload).second) {
            benchmarkWorkloads.push_back(e.workload);
        }
    }
    Json j = {
        {"openmp", {
            {"compiled", caps.openmp_compiled},
            {"runtime", caps.openmp_runtime},
        }},
        {"avx2", {
            {"compiled", caps.avx2_compiled},
            {"runtime", caps.avx2_runtime},
            {"fma", caps.fma_runtime},
        }},
        {"bmi2", {
            {"runtime", caps.bmi2_runtime},
        }},
        {"avx512", {
            {"compiled", caps.avx512_compiled},
            {"runtime", caps.avx512_runtime},
            {"ifma", caps.avx512ifma_runtime},
        }},
        {"cuda", {
            {"compiled", caps.cuda_compiled},
            {"runtime", caps.cuda_runtime},
            {"lowEnd", caps.cuda_low_end},
            {"deviceCount", caps.cuda_device_count},
            {"name", caps.cuda_name},
            {"computeCapability", {
                {"major", caps.cuda_compute_major},
                {"minor", caps.cuda_compute_minor},
            }},
            {"vram", {
                {"totalBytes", caps.cuda_total_vram},
                {"freeBytes", caps.cuda_free_vram},
                {"totalMiB", caps.cuda_total_vram / (1024 * 1024)},
                {"freeMiB", caps.cuda_free_vram / (1024 * 1024)},
            }},
        }},
        {"cpu", {
            {"logicalCores", caps.logical_cores},
            {"physicalCores", caps.physical_cores},
            {"thermalFriendly", compute::thermal_friendly_mode()},
        }},
        {"benchmarkCache", {
            {"available", availableProfiles > 0},
            {"profileVersion", 1},
            {"entryCount", benchmarkEntries.size()},
            {"availableEntryCount", availableProfiles},
            {"sampleCount", benchmarkSamples},
            {"families", benchmarkFamilies},
            {"workloads", benchmarkWorkloads},
            {"results", bench},
        }},
    };
    return j.dump();
}

} // namespace fsd
