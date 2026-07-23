// routes_mesh.cpp
//
// Real implementations of:
//   POST /api/hs/mesh         — heightfield mesh for any HS metric × variant
//   POST /api/transition/mesh — marching-cubes mesh of the 3D transition set

#include "routes.hpp"
#include "routes_common.hpp"
#include "resource_manager.hpp"

#include "../compute/hs/heightfield_mesh.hpp"
#include "../compute/engine_select.hpp"
#include "../compute/transition_volume.hpp"
#include "../compute/marching_cubes.hpp"
#include "../compute/mesh_io.hpp"
#include "../compute/variants.hpp"
#include "../compute/escape_time.hpp"
#include "../compute/orbit_program_json.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <cstring>
#include <vector>

namespace fsd {

namespace {

compute::Variant parseVariant(const std::string& s) {
    compute::Variant v;
    if (compute::variant_from_name(s.c_str(), v)) return v;
    return compute::Variant::Mandelbrot;
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
            legs.push_back({parseVariant(variant), weight});
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
            legs.push_back({parseVariant(variant), weight});
        }
    }
    return legs;
}

compute::Metric parseMetric(const std::string& s) {
    compute::Metric m;
    if (compute::metric_from_name(s.c_str(), m)) return m;
    return compute::Metric::MinAbs;
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

uint64_t estimateTransitionBytes(int resolution, bool voxelPreview) {
    const uint64_t n = static_cast<uint64_t>(resolution);
    const uint64_t voxels = n * n * n;
    const uint64_t field = voxels * sizeof(float);
    const uint64_t voxelMasks = voxelPreview ? voxels * 2u : 0u;
    const uint64_t working = field;
    return field + voxelMasks + working;
}

void validateTransitionResolution(const Json& j, int resolution, bool voxelPreview) {
    if (resolution < 4 || resolution > 1024) throw std::runtime_error("resolution out of range [4,1024]");
    const bool allowLarge = j.value("allowLargeVolume", false);
    if (resolution >= 512 && !allowLarge) {
        throw std::runtime_error("512^3 transition volumes are disabled by default; set allowLargeVolume=true after checking memory");
    }
    if (resolution >= 256) {
        const uint64_t estimate = estimateTransitionBytes(resolution, voxelPreview);
        const auto caps = compute::runtime_capabilities();
        if (caps.cuda_runtime && caps.cuda_free_vram > 0 && estimate > caps.cuda_free_vram * 7ULL / 10ULL) {
            throw std::runtime_error("transition volume estimate exceeds safe CUDA VRAM budget");
        }
        if (!allowLarge && estimate > 2ULL * 1024ULL * 1024ULL * 1024ULL) {
            throw std::runtime_error("transition volume estimate exceeds 2 GiB; set allowLargeVolume=true to override");
        }
    }
}

int defaultTransitionResolution(bool voxelPreview) {
    if (voxelPreview) return 128;
    const auto caps = compute::runtime_capabilities();
    return caps.cuda_low_end ? 128 : 192;
}

void setTransitionProgress(
    JobRunner& runner,
    const std::string& runId,
    const std::string& stage,
    int current,
    int total,
    const std::string& engine,
    const std::string& scalar,
    bool cancelable = false,
    bool kernelReported = false
) {
    const double percent = total > 0
        ? (100.0 * static_cast<double>(current) / static_cast<double>(total))
        : 0.0;
    Json j = {
        {"taskType", "transition_volume"},
        {"stage", stage},
        {"current", current},
        {"total", total},
        {"percent", percent},
        {"engine", engine},
        {"scalar", scalar},
        {"kernelReported", kernelReported},
        {"elapsedMs", runner.runElapsedMs(runId)},
        {"estimatedRemainingMs", nullptr},
        {"cancelable", cancelable},
        {"resourceLocks", Json::array({"transition_volume", "cuda_heavy", "cpu_heavy"})},
        {"details", Json::object()},
    };
    runner.setProgress(runId, j.dump());
}

ResourceManager::Lease acquireTransitionLease(JobRunner& runner, const std::string& runId) {
    ResourceManager::Lease lease;
    std::string conflictLock, activeRunId;
    if (!resourceManager().tryAcquire(runId, "transition_volume", {"transition_volume", "cuda_heavy", "cpu_heavy"}, lease, conflictLock, activeRunId)) {
        runner.setStatus(runId, "failed");
        throw HttpError(409, Json{
            {"error", "transition_volume already running"},
            {"activeRunId", activeRunId},
            {"taskType", "transition_volume"},
            {"resourceLock", conflictLock},
        }.dump());
    }
    return lease;
}

} // namespace

std::string hsMeshRoute(const std::filesystem::path&, JobRunner& runner, const std::string& body) {
    const Json j = parseJsonBody(body);

    compute::hs::HsMeshParams p;
    p.center_re = j.value("centerRe",  -0.75);
    p.center_im = j.value("centerIm",    0.0);
    p.scale     = j.value("scale",       3.0);
    p.resolution = j.value("resolution", 192);
    p.iterations = j.value("iterations", 512);
    p.heightScale = j.value("heightScale", 0.6);
    p.heightClamp = j.value("heightClamp", 2.0);
    p.pairwise_cap = j.value("pairwiseCap", 64);
    p.variant = parseVariant(j.value("variant", std::string("mandelbrot")));
    p.bailout = j.contains("bailout") && !j["bailout"].is_null()
        ? j.value("bailout", 2.0)
        : compute::variant_default_bailout(p.variant);
    p.bailout_sq = bailoutSqFromJson(j, p.bailout, compute::variant_default_bailout_sq(p.variant));
    if (j.contains("bailoutSq") && !j["bailoutSq"].is_null() &&
        !(j.contains("bailout") && !j["bailout"].is_null())) {
        p.bailout = std::sqrt(p.bailout_sq);
    }
    p.metric  = parseMetric (j.value("metric",  std::string("min_abs")));
    if (j.contains("orbitProgram") && !j["orbitProgram"].is_null()) {
        p.orbit_program = compute::parse_orbit_program_json(j["orbitProgram"]);
    }

    if (p.resolution < 8 || p.resolution > 4096) throw std::runtime_error("invalid resolution");
    if (p.iterations < 1 || p.iterations > 1000000) throw std::runtime_error("invalid iterations");
    if (p.pairwise_cap < 1 || p.pairwise_cap > 1000000) throw std::runtime_error("invalid pairwiseCap");
    if (!(p.bailout > 0.0) || !std::isfinite(p.bailout)) throw std::runtime_error("invalid bailout");
    if (!(p.bailout_sq > 0.0) || !std::isfinite(p.bailout_sq)) throw std::runtime_error("invalid bailoutSq");

    auto run = runner.createRun("hs-mesh", body);
    const bool background = j.value("background", false);
    const auto cancelToken = runner.cancelToken(run.id);
    p.should_cancel = [cancelToken]() {
        return cancelToken->load(std::memory_order_relaxed);
    };
    runner.setCancelable(run.id, true);
    const std::string actualEngine = p.orbit_program ? "openmp_orbit" : "openmp";
    auto setProgress = [&runner, run, actualEngine](const char* stage, int current,
                                                    bool kernelReported) {
        runner.setProgress(run.id, Json{
            {"taskType", "hs_mesh"}, {"stage", stage},
            {"current", current}, {"total", 1}, {"percent", 100.0 * current},
            {"engine", actualEngine}, {"scalar", "fp64"},
            {"kernelReported", kernelReported},
            {"elapsedMs", runner.runElapsedMs(run.id)},
            {"cancelable", !kernelReported},
            {"resourceLocks", Json::array({"cpu_heavy"})},
            {"details", Json::object()},
        }.dump());
    };
    setProgress("queued", 0, false);

    struct HsMeshResult {
        double elapsed = 0.0;
        size_t vertexCount = 0;
        size_t triangleCount = 0;
    };
    auto execute = [=, &runner]() mutable -> HsMeshResult {
        runner.setStatus(run.id, "running");
        setProgress("field_and_mesh", 0, false);
        HsMeshResult result;
        try {
        const auto t0 = std::chrono::steady_clock::now();
        compute::Mesh mesh = compute::hs::buildHsMesh(p);
        const auto t1 = std::chrono::steady_clock::now();
        result.elapsed = std::chrono::duration<double, std::milli>(t1 - t0).count();
        result.vertexCount = mesh.vertices.size();
        result.triangleCount = mesh.triangleCount();

        const std::filesystem::path glbPath =
            std::filesystem::path(run.outputDir) / "hs_mesh.glb";
        const std::filesystem::path stlPath =
            std::filesystem::path(run.outputDir) / "hs_mesh.stl";
        compute::writeGlb(glbPath.string(), mesh);
        compute::writeStlBinary(stlPath.string(), mesh);

        runner.addArtifact(run.id, Artifact{"hs-mesh", glbPath.string(), "mesh"});
        runner.addArtifact(run.id, Artifact{"hs-mesh", stlPath.string(), "stl"});
        setProgress("completed", 1, true);
        runner.setCancelable(run.id, false);
        runner.setStatus(run.id, "completed");
        return result;
        } catch (const std::exception& error) {
            runner.setCancelable(run.id, false);
            if (std::string(error.what()) == "cancelled") {
                setProgress("cancelled", 0, false);
                runner.setStatus(run.id, "cancelled");
            } else {
                setProgress("failed", 0, false);
                runner.setStatus(run.id, "failed");
            }
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

    const HsMeshResult result = execute();

    const std::string glbId = run.id + ":hs_mesh.glb";
    const std::string stlId = run.id + ":hs_mesh.stl";
    Json resp = {
        {"runId",      run.id},
        {"status",     "completed"},
        {"glbArtifactId", glbId},
        {"stlArtifactId", stlId},
        {"glbUrl",     "/api/artifacts/content?artifactId=" + glbId},
        {"stlUrl",     "/api/artifacts/download?artifactId=" + stlId},
        {"vertexCount", result.vertexCount},
        {"triangleCount", result.triangleCount},
        {"generatedMs", result.elapsed},
    };
    return resp.dump();
}

// ─── HS field (raw float64 height values for frontend-rendered mesh) ─────────
// Returns the same field that buildHsMesh computes internally, before the
// meshing step, so the browser can build a three.js PlaneGeometry and apply an
// arbitrary z-scale without a round-trip to the server.

std::string hsFieldRoute(const std::filesystem::path&, JobRunner& runner, const std::string& body) {
    const Json j = parseJsonBody(body);

    compute::hs::HsMeshParams p;
    p.center_re   = j.value("centerRe",    -0.75);
    p.center_im   = j.value("centerIm",      0.0);
    p.scale       = j.value("scale",         3.0);
    p.resolution  = j.value("resolution",   192);
    p.iterations  = j.value("iterations",   512);
    p.heightClamp = j.value("heightClamp",   2.0);
    p.pairwise_cap = j.value("pairwiseCap", 64);
    p.variant = parseVariant(j.value("variant", std::string("mandelbrot")));
    p.bailout = j.contains("bailout") && !j["bailout"].is_null()
        ? j.value("bailout", 2.0)
        : compute::variant_default_bailout(p.variant);
    p.bailout_sq = bailoutSqFromJson(j, p.bailout, compute::variant_default_bailout_sq(p.variant));
    if (j.contains("bailoutSq") && !j["bailoutSq"].is_null() &&
        !(j.contains("bailout") && !j["bailout"].is_null())) {
        p.bailout = std::sqrt(p.bailout_sq);
    }
    p.metric  = parseMetric (j.value("metric",  std::string("min_abs")));
    if (j.contains("orbitProgram") && !j["orbitProgram"].is_null()) {
        p.orbit_program = compute::parse_orbit_program_json(j["orbitProgram"]);
    }

    if (p.resolution < 8 || p.resolution > 4096) throw std::runtime_error("invalid resolution");
    if (p.iterations < 1 || p.iterations > 1000000) throw std::runtime_error("invalid iterations");
    if (p.pairwise_cap < 1 || p.pairwise_cap > 1000000) throw std::runtime_error("invalid pairwiseCap");
    if (!(p.bailout > 0.0) || !std::isfinite(p.bailout)) throw std::runtime_error("invalid bailout");
    if (!(p.bailout_sq > 0.0) || !std::isfinite(p.bailout_sq)) throw std::runtime_error("invalid bailoutSq");

    auto run = runner.createRun("hs-field", body);
    const bool background = j.value("background", false);
    const auto cancelToken = runner.cancelToken(run.id);
    p.should_cancel = [cancelToken]() {
        return cancelToken->load(std::memory_order_relaxed);
    };
    runner.setCancelable(run.id, true);
    const std::string actualEngine = p.orbit_program ? "openmp_orbit" : "openmp";
    auto setProgress = [&runner, run, actualEngine](const char* stage, int current,
                                                    bool kernelReported) {
        runner.setProgress(run.id, Json{
            {"taskType", "hs_field"}, {"stage", stage},
            {"current", current}, {"total", 1}, {"percent", 100.0 * current},
            {"engine", actualEngine}, {"scalar", "fp64"},
            {"kernelReported", kernelReported},
            {"elapsedMs", runner.runElapsedMs(run.id)},
            {"cancelable", !kernelReported},
            {"resourceLocks", Json::array({"cpu_heavy"})},
            {"details", Json::object()},
        }.dump());
    };
    setProgress("queued", 0, false);

    struct HsFieldResult {
        std::vector<double> values;
        double minimum = 0.0;
        double maximum = 0.0;
        double elapsed = 0.0;
    };
    auto execute = [=, &runner]() mutable -> HsFieldResult {
        runner.setStatus(run.id, "running");
        setProgress("field", 0, false);
        HsFieldResult result;
        try {
        const auto t0 = std::chrono::steady_clock::now();

        // Reuse the existing field compute from buildHsMesh, which computes
        // raw metric values. We replicate the field computation here to return
        // the raw (un-normalized, un-meshed) float64 values. The frontend does
        // normalization and meshing.
        compute::hs::computeHsField(p, result.values);

        const auto t1 = std::chrono::steady_clock::now();
        result.elapsed = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // Compute min/max for the frontend (it normalizes for colorization).
        result.minimum =  std::numeric_limits<double>::infinity();
        result.maximum = -std::numeric_limits<double>::infinity();
        for (double value : result.values) {
            if (value < result.minimum) result.minimum = value;
            if (value > result.maximum) result.maximum = value;
        }

        const std::filesystem::path fieldPath =
            std::filesystem::path(run.outputDir) / "hs_field.f64";
        {
            std::ofstream output(fieldPath, std::ios::binary);
            if (!output) throw std::runtime_error("failed to open HS field artifact");
            output.write(reinterpret_cast<const char*>(result.values.data()),
                         static_cast<std::streamsize>(result.values.size() * sizeof(double)));
            if (!output) throw std::runtime_error("failed to write HS field artifact");
        }
        const std::filesystem::path metadataPath =
            std::filesystem::path(run.outputDir) / "hs_field.json";
        {
            std::ofstream output(metadataPath);
            output << Json{
                {"schemaVersion", 1}, {"dataType", "float64"},
                {"byteOrder", "little_endian"},
                {"shape", Json::array({p.resolution, p.resolution})},
                {"fieldMin", result.minimum}, {"fieldMax", result.maximum},
                {"engine", actualEngine}, {"scalar", "fp64"},
            }.dump(2);
            if (!output) throw std::runtime_error("failed to write HS field metadata");
        }
        runner.addArtifact(run.id, Artifact{"hs-field", fieldPath.string(), "field"});
        runner.addArtifact(run.id, Artifact{"hs-field", metadataPath.string(), "report"});
        setProgress("completed", 1, true);
        runner.setCancelable(run.id, false);
        runner.setStatus(run.id, "completed");
        return result;
        } catch (const std::exception& error) {
            runner.setCancelable(run.id, false);
            if (std::string(error.what()) == "cancelled") {
                setProgress("cancelled", 0, false);
                runner.setStatus(run.id, "cancelled");
            } else {
                setProgress("failed", 0, false);
                runner.setStatus(run.id, "failed");
            }
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

    const HsFieldResult result = execute();
    const auto* bytes = reinterpret_cast<const uint8_t*>(result.values.data());
    return Json{
        {"runId", run.id}, {"status", "completed"},
        {"width", p.resolution}, {"height", p.resolution},
        {"fieldMin", result.minimum}, {"fieldMax", result.maximum},
        {"fieldB64", base64Encode(bytes, result.values.size() * sizeof(double))},
        {"generatedMs", result.elapsed},
    }.dump();
}

// Transition 3D volume mesh (Mandelbrot ↔ Burning Ship bridge as a 3D object).
std::string transitionMeshRoute(const std::filesystem::path&, JobRunner& runner, const std::string& body) {
    const Json j = parseJsonBody(body);

    compute::TransitionVolumeParams p;
    p.centerX   = j.value("centerX",   0.0);
    p.centerY   = j.value("centerY",   0.0);
    p.centerZ   = j.value("centerZ",   0.0);
    p.extent    = j.value("extent",    2.0);
    p.resolution = j.value("resolution", defaultTransitionResolution(false));
    p.iterations = j.value("iterations", 256);
    p.bailout   = j.value("bailout",   2.0);
    p.bailout_sq = bailoutSqFromJson(j, p.bailout, 4.0);
    if (j.contains("bailoutSq") && !j["bailoutSq"].is_null() &&
        !(j.contains("bailout") && !j["bailout"].is_null())) {
        p.bailout = std::sqrt(p.bailout_sq);
    }
    p.from_variant = parseVariant(j.value("transitionFrom", std::string("mandelbrot")));
    p.to_variant   = parseVariant(j.value("transitionTo",   std::string("burning_ship")));
    p.multi_legs    = parseTransitionLegs(j);
    p.engine        = j.value("engine", std::string("auto"));
    p.scalar_type   = j.value("scalarType", std::string("fp32"));
    const double iso = j.value("iso",  0.5);

    validateTransitionResolution(j, p.resolution, false);
    if (p.resolution < 8) throw std::runtime_error("invalid resolution");
    if (p.iterations < 1 || p.iterations > 10000) throw std::runtime_error("invalid iterations");
    if (!(p.bailout > 0.0) || !std::isfinite(p.bailout)) throw std::runtime_error("invalid bailout");
    if (!(p.bailout_sq > 0.0) || !std::isfinite(p.bailout_sq)) throw std::runtime_error("invalid bailoutSq");

    auto run = runner.createRun("transition-mesh", body);
    const bool background = j.value("background", false);
    auto transitionLease = std::make_shared<ResourceManager::Lease>(
        acquireTransitionLease(runner, run.id));
    const auto cancelToken = runner.cancelToken(run.id);
    p.should_cancel = [cancelToken]() {
        return cancelToken->load(std::memory_order_relaxed);
    };
    runner.setCancelable(run.id, true);
    setTransitionProgress(runner, run.id, "queued", 0, 2,
                          p.engine, p.scalar_type, true);

    struct TransitionMeshResult {
        double fieldMs = 0.0;
        double marchingCubesMs = 0.0;
        size_t vertexCount = 0;
        size_t triangleCount = 0;
        std::string engine = "openmp_fp32";
        std::string scalar = "fp32";
    };
    auto execute = [=, &runner]() mutable -> TransitionMeshResult {
        (void)transitionLease;
        runner.setStatus(run.id, "running");
        setTransitionProgress(runner, run.id, "volume", 0, 2,
                              p.engine, p.scalar_type, true);
        TransitionMeshResult result;
        try {
        const auto t0 = std::chrono::steady_clock::now();
        compute::McField field = compute::buildTransitionVolume(p);
        result.engine = field.engine_used;
        result.scalar = field.scalar_used;
        const auto t1 = std::chrono::steady_clock::now();
        setTransitionProgress(runner, run.id, "marching_cubes", 1, 2,
                              result.engine, result.scalar, true);
        if (p.should_cancel()) throw std::runtime_error("cancelled");
        compute::Mesh mesh = compute::marchingCubes(field, static_cast<float>(iso));
        if (p.should_cancel()) throw std::runtime_error("cancelled");
        const auto t2 = std::chrono::steady_clock::now();
        result.fieldMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        result.marchingCubesMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
        result.vertexCount = mesh.vertices.size();
        result.triangleCount = mesh.triangleCount();

        if (result.vertexCount == 0) {
            throw std::runtime_error("empty mesh (iso value gives no surface)");
        }

        const std::filesystem::path glbPath =
            std::filesystem::path(run.outputDir) / "transition_mesh.glb";
        const std::filesystem::path stlPath =
            std::filesystem::path(run.outputDir) / "transition_mesh.stl";
        compute::writeGlb(glbPath.string(), mesh);
        compute::writeStlBinary(stlPath.string(), mesh);

        runner.addArtifact(run.id, Artifact{"transition-mesh", glbPath.string(), "mesh"});
        runner.addArtifact(run.id, Artifact{"transition-mesh", stlPath.string(), "stl"});
        setTransitionProgress(runner, run.id, "completed", 2, 2,
                              result.engine, result.scalar, false, true);
        runner.setCancelable(run.id, false);
        runner.setStatus(run.id, "completed");
        return result;
        } catch (const std::exception& error) {
            runner.setCancelable(run.id, false);
            if (std::string(error.what()) == "cancelled") {
                setTransitionProgress(runner, run.id, "cancelled", 0, 2,
                                      result.engine, result.scalar);
                runner.setStatus(run.id, "cancelled");
            } else {
                setTransitionProgress(runner, run.id, "failed", 0, 2,
                                      result.engine, result.scalar);
                runner.setStatus(run.id, "failed");
            }
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

    const TransitionMeshResult result = execute();

    const std::string glbId = run.id + ":transition_mesh.glb";
    const std::string stlId = run.id + ":transition_mesh.stl";
    Json resp = {
        {"runId",      run.id},
        {"status",     "completed"},
        {"glbArtifactId", glbId},
        {"stlArtifactId", stlId},
        {"glbUrl",     "/api/artifacts/content?artifactId=" + glbId},
        {"stlUrl",     "/api/artifacts/download?artifactId=" + stlId},
        {"vertexCount", result.vertexCount},
        {"triangleCount", result.triangleCount},
        {"fieldMs",  result.fieldMs},
        {"mcMs",     result.marchingCubesMs},
        {"fieldEngineUsed", result.engine},
        {"fieldScalarUsed", result.scalar},
    };
    return resp.dump();
}

// Voxel grid export for the Minecraft-style 3D transition renderer.
//
// Face culling happens here in C++: for each inside voxel, only faces adjacent
// to an outside neighbour are emitted (the user's v[n][n][n] 0↔1 algorithm).
// This eliminates all hidden geometry before it ever leaves the server.
//
// Response carries three base64-encoded flat arrays:
//   posB64   — float32[faceCount * 4 * 3]   world-space vertex positions
//   normB64  — int8[faceCount * 3]           one outward normal per face (−1/0/+1)
//   depthB64 — uint8[faceCount]              depth byte per face (1=deep, 255=surface)
//
// CCW winding per face is chosen so the outward normal equals n × (v1−v0) × (v2−v0).
std::string transitionVoxelsRoute(const std::filesystem::path&, JobRunner& runner, const std::string& body) {
    const Json j = parseJsonBody(body);

    compute::TransitionVolumeParams p;
    p.centerX    = j.value("centerX",    0.0);
    p.centerY    = j.value("centerY",    0.0);
    p.centerZ    = j.value("centerZ",    0.0);
    p.extent     = j.value("extent",     2.0);
    p.resolution = j.value("resolution", defaultTransitionResolution(true));
    p.iterations = j.value("iterations", 128);
    p.bailout    = j.value("bailout",    2.0);
    p.bailout_sq = bailoutSqFromJson(j, p.bailout, 4.0);
    if (j.contains("bailoutSq") && !j["bailoutSq"].is_null() &&
        !(j.contains("bailout") && !j["bailout"].is_null())) {
        p.bailout = std::sqrt(p.bailout_sq);
    }
    p.from_variant = parseVariant(j.value("transitionFrom", std::string("mandelbrot")));
    p.to_variant   = parseVariant(j.value("transitionTo",   std::string("burning_ship")));
    p.multi_legs    = parseTransitionLegs(j);
    p.engine        = j.value("engine", std::string("auto"));
    p.scalar_type   = j.value("scalarType", std::string("fp32"));
    const float iso = static_cast<float>(j.value("iso", 0.48));

    validateTransitionResolution(j, p.resolution, true);
    if (p.iterations < 1 || p.iterations > 10000) throw std::runtime_error("invalid iterations");
    if (!(p.bailout > 0.0) || !std::isfinite(p.bailout)) throw std::runtime_error("invalid bailout");
    if (!(p.bailout_sq > 0.0) || !std::isfinite(p.bailout_sq)) throw std::runtime_error("invalid bailoutSq");

    auto run = runner.createRun("transition-voxels", body);
    const bool background = j.value("background", false);
    auto transitionLease = std::make_shared<ResourceManager::Lease>(
        acquireTransitionLease(runner, run.id));
    const auto cancelToken = runner.cancelToken(run.id);
    p.should_cancel = [cancelToken]() {
        return cancelToken->load(std::memory_order_relaxed);
    };
    runner.setCancelable(run.id, true);
    setTransitionProgress(runner, run.id, "queued", 0, 2,
                          p.engine, p.scalar_type, true);

    auto execute = [=, &runner]() mutable -> Json {
        (void)transitionLease;
        runner.setStatus(run.id, "running");
        setTransitionProgress(runner, run.id, "volume", 0, 2,
                              p.engine, p.scalar_type, true);
        compute::McField field;
        try {
        const auto t0 = std::chrono::steady_clock::now();
        field = compute::buildTransitionVolume(p);
        setTransitionProgress(runner, run.id, "voxel_mesh", 1, 2,
                              field.engine_used, field.scalar_used, true);
        const auto t1 = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double, std::milli>(t1 - t0).count();

    const int N = field.Nx;

    // Binary inside/outside volume + per-voxel depth byte
    std::vector<uint8_t> vol(static_cast<size_t>(N) * N * N, 0);
    std::vector<uint8_t> dep(static_cast<size_t>(N) * N * N, 0);
    size_t voxelCount = 0;
    for (int i = 0; i < N * N * N; ++i) {
        if ((i & 65535) == 0 && p.should_cancel()) {
            throw std::runtime_error("cancelled");
        }
        const float v = field.data[i];
        if (v < iso) {
            vol[i] = 1;
            voxelCount++;
            dep[i] = static_cast<uint8_t>(std::min(255, 1 + static_cast<int>(v / iso * 254.0f)));
        }
    }

    // 6 face definitions — CCW winding verified by cross-product for outward normal.
    // Each face: neighbour delta (dx,dy,dz), outward normal (nx,ny,nz),
    //            4 corner offsets in half-cell units relative to voxel centre.
    struct FaceDef { int dx, dy, dz; int8_t nx, ny, nz; float v[4][3]; };
    static const FaceDef FACES[6] = {
        {  1, 0, 0,  1, 0, 0, {{ 0.5f,-0.5f,-0.5f},{ 0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f, 0.5f},{ 0.5f,-0.5f, 0.5f}} },
        { -1, 0, 0, -1, 0, 0, {{-0.5f,-0.5f, 0.5f},{-0.5f, 0.5f, 0.5f},{-0.5f, 0.5f,-0.5f},{-0.5f,-0.5f,-0.5f}} },
        {  0, 1, 0,  0, 1, 0, {{-0.5f, 0.5f,-0.5f},{-0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f,-0.5f}} },
        {  0,-1, 0,  0,-1, 0, {{-0.5f,-0.5f, 0.5f},{-0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f, 0.5f}} },
        {  0, 0, 1,  0, 0, 1, {{-0.5f,-0.5f, 0.5f},{ 0.5f,-0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{-0.5f, 0.5f, 0.5f}} },
        {  0, 0,-1,  0, 0,-1, {{ 0.5f,-0.5f,-0.5f},{-0.5f,-0.5f,-0.5f},{-0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f,-0.5f}} },
    };

    const float cellSize = static_cast<float>(p.extent * 2.0) / static_cast<float>(N);
    const float origin   = static_cast<float>(-p.extent) + cellSize * 0.5f;

    auto getVol = [&](int xi, int yi, int zi) -> uint8_t {
        if (xi < 0 || xi >= N || yi < 0 || yi >= N || zi < 0 || zi >= N) return 0;
        return vol[xi + N * (yi + N * zi)];
    };

    // Accumulate exposed-face geometry (only 0→1 or 1→0 transitions, i.e. inside↔outside)
    std::vector<float>   posF32;
    std::vector<int8_t>  normI8;
    std::vector<uint8_t> depU8;

    for (int zi = 0; zi < N; ++zi) {
        if (p.should_cancel()) throw std::runtime_error("cancelled");
        for (int yi = 0; yi < N; ++yi) {
            for (int xi = 0; xi < N; ++xi) {
                if (!getVol(xi, yi, zi)) continue;  // outside — skip

                const uint8_t depth = dep[xi + N * (yi + N * zi)];
                const float cx = origin + static_cast<float>(xi) * cellSize;
                const float cy = origin + static_cast<float>(yi) * cellSize;
                const float cz = origin + static_cast<float>(zi) * cellSize;

                for (const auto& f : FACES) {
                    // Emit face only when neighbour is outside (inside↔outside boundary)
                    if (getVol(xi + f.dx, yi + f.dy, zi + f.dz)) continue;

                    for (int k = 0; k < 4; ++k) {
                        posF32.push_back(cx + f.v[k][0] * cellSize);
                        posF32.push_back(cy + f.v[k][1] * cellSize);
                        posF32.push_back(cz + f.v[k][2] * cellSize);
                    }
                    normI8.push_back(f.nx);
                    normI8.push_back(f.ny);
                    normI8.push_back(f.nz);
                    depU8.push_back(depth);
                }
            }
        }
    }

    const size_t faceCount = depU8.size();

    // Write binary STL: each quad (4 verts) → 2 triangles.
    // Binary STL: 80-byte header | uint32 triCount | per-tri: float[3] normal, float[3][3] verts, uint16=0
    const std::filesystem::path stlPath =
        std::filesystem::path(run.outputDir) / "transition_voxels.stl";
    {
        const uint32_t triCount = static_cast<uint32_t>(faceCount * 2);
        const std::filesystem::path tmpPath = stlPath.string() + ".tmp";
        std::ofstream stlOut(tmpPath, std::ios::binary);
        // 80-byte header
        char header[80] = {};
        std::memcpy(header, "fractal_studio voxel STL", 24);
        stlOut.write(header, 80);
        stlOut.write(reinterpret_cast<const char*>(&triCount), 4);
        const uint16_t attr = 0;
        for (size_t fi = 0; fi < faceCount; ++fi) {
            if ((fi & 4095U) == 0U && p.should_cancel()) {
                throw std::runtime_error("cancelled");
            }
            // Normal (float32 × 3)
            const float nx = static_cast<float>(normI8[fi * 3 + 0]);
            const float ny = static_cast<float>(normI8[fi * 3 + 1]);
            const float nz = static_cast<float>(normI8[fi * 3 + 2]);
            // 4 vertices for this quad
            const float* v = &posF32[fi * 12];
            // Triangle 0: v0, v1, v2
            stlOut.write(reinterpret_cast<const char*>(&nx), 4);
            stlOut.write(reinterpret_cast<const char*>(&ny), 4);
            stlOut.write(reinterpret_cast<const char*>(&nz), 4);
            stlOut.write(reinterpret_cast<const char*>(v + 0), 12); // v0
            stlOut.write(reinterpret_cast<const char*>(v + 3), 12); // v1
            stlOut.write(reinterpret_cast<const char*>(v + 6), 12); // v2
            stlOut.write(reinterpret_cast<const char*>(&attr), 2);
            // Triangle 1: v0, v2, v3
            stlOut.write(reinterpret_cast<const char*>(&nx), 4);
            stlOut.write(reinterpret_cast<const char*>(&ny), 4);
            stlOut.write(reinterpret_cast<const char*>(&nz), 4);
            stlOut.write(reinterpret_cast<const char*>(v + 0),  12); // v0
            stlOut.write(reinterpret_cast<const char*>(v + 6),  12); // v2
            stlOut.write(reinterpret_cast<const char*>(v + 9),  12); // v3
            stlOut.write(reinterpret_cast<const char*>(&attr), 2);
        }
        stlOut.close();
        if (!stlOut) throw std::runtime_error("failed to write transition voxel STL");
        std::error_code ec;
        std::filesystem::rename(tmpPath, stlPath, ec);
        if (ec) {
            std::filesystem::remove(stlPath, ec);
            ec.clear();
            std::filesystem::rename(tmpPath, stlPath, ec);
        }
        if (ec) {
            std::filesystem::remove(tmpPath, ec);
            throw std::runtime_error("failed to finalize transition voxel STL");
        }
    }
    runner.addArtifact(run.id, Artifact{"transition-voxels", stlPath.string(), "stl"});
    setTransitionProgress(runner, run.id, "completed", 2, 2,
                          field.engine_used, field.scalar_used, false, true);
    runner.setCancelable(run.id, false);
    runner.setStatus(run.id, "completed");

    const std::string stlId = run.id + ":transition_voxels.stl";
    Json resp = {
        {"runId",       run.id},
        {"status",      "completed"},
        {"resolution",  N},
        {"extent",      p.extent},
        {"voxelCount",  voxelCount},
        {"faceCount",   faceCount},
        {"generatedMs", elapsed},
        {"fieldEngineUsed", field.engine_used},
        {"fieldScalarUsed", field.scalar_used},
        {"stlArtifactId", stlId},
        {"stlUrl",      "/api/artifacts/download?artifactId=" + stlId},
        {"posB64",   base64Encode(reinterpret_cast<const uint8_t*>(posF32.data()), posF32.size() * sizeof(float))},
        {"normB64",  base64Encode(reinterpret_cast<const uint8_t*>(normI8.data()), normI8.size())},
        {"depthB64", base64Encode(depU8.data(), depU8.size())},
    };
    return resp;
        } catch (const std::exception& error) {
            std::error_code cleanupError;
            std::filesystem::remove(
                std::filesystem::path(run.outputDir) / "transition_voxels.stl.tmp",
                cleanupError);
            runner.setCancelable(run.id, false);
            const std::string engine = field.engine_used.empty() ? p.engine : field.engine_used;
            const std::string scalar = field.scalar_used.empty() ? p.scalar_type : field.scalar_used;
            if (std::string(error.what()) == "cancelled") {
                setTransitionProgress(runner, run.id, "cancelled", 0, 2, engine, scalar);
                runner.setStatus(run.id, "cancelled");
            } else {
                setTransitionProgress(runner, run.id, "failed", 0, 2, engine, scalar);
                runner.setStatus(run.id, "failed");
            }
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
