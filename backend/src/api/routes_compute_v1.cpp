#include "routes.hpp"
#include "routes_common.hpp"

#include "../include/db.hpp"
#include "../compute/engine_select.hpp"
#include "../compute/orbit_program_json.hpp"

#include <openssl/evp.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fsd {
namespace {

constexpr int COMPUTE_SCHEMA_VERSION = 1;
std::mutex g_idempotencyMutex;
std::map<std::string, std::weak_ptr<std::mutex>> g_idempotencyLocks;

struct ComputeCapability {
    const char* kind;
    bool persistent;
    bool preview;
    bool orbitPayload;
    const char* variantProfile;
    const char* metrics;
    const char* engines;
    const char* scalars;
    const char* outputMediaTypes;
};

constexpr std::array<ComputeCapability, 18> COMPUTE_CAPABILITIES = {{
    {"map_image", true, true, true, "builtin_2d_or_safe_dsl", "escape,min_abs,max_abs,envelope,min_pairwise_dist,mandel_ship_agree", "auto,openmp,avx2,avx512,cuda,hybrid", "auto,fp32,fp64,fx64,fp80,fp128", "image/png,application/octet-stream"},
    {"raw_field", false, true, true, "builtin_2d_or_safe_dsl", "escape,min_abs,max_abs,envelope,min_pairwise_dist", "auto,openmp,avx2,avx512,cuda,hybrid", "auto,fp32,fp64,fx64,fp80,fp128", "application/json"},
    {"ln_map", true, false, true, "builtin_2d_or_safe_dsl", "escape", "auto,openmp,avx2,avx512,cuda", "auto,fp32,fp64,fx64,fp80,fp128", "image/png,application/json"},
    {"zoom_video", true, false, true, "builtin_2d_or_safe_dsl", "escape", "auto,openmp,avx2,avx512,cuda,hybrid", "auto,fp32,fp64,fx64,fp80,fp128", "video/mp4,image/png,application/json"},
    {"legacy_zoom_video", true, false, false, "ln_map_sidecar_snapshot", "escape", "auto,openmp,avx2,avx512,cuda,hybrid", "auto,fp32,fp64,fx64,fp80,fp128", "video/mp4"},
    {"video_preview", false, true, true, "builtin_2d_or_safe_dsl", "escape", "auto,openmp", "fp64", "application/json,image/png"},
    {"transition_video", true, false, false, "axis_pair_or_multi", "escape,min_abs,max_abs,envelope", "auto,openmp,avx2,cuda", "auto,fp32,fp64,fx64,fp80,fp128", "video/mp4,image/png,application/json"},
    {"transition_video_preview", false, true, false, "axis_pair_or_multi", "escape,min_abs,max_abs,envelope", "auto,openmp,avx2,cuda", "auto,fp32,fp64,fx64,fp80,fp128", "application/json,image/png"},
    {"hs_mesh", true, false, true, "builtin_2d_or_safe_dsl", "escape,min_abs,max_abs,envelope,min_pairwise_dist", "auto,openmp", "fp64", "model/gltf-binary,application/sla"},
    {"hs_field", true, false, true, "builtin_2d_or_safe_dsl", "escape,min_abs,max_abs,envelope,min_pairwise_dist", "auto,openmp", "fp64", "application/octet-stream,application/json"},
    {"transition_mesh", true, false, false, "axis_pair_or_multi", "escape", "auto,openmp,avx2,cuda", "auto,fp32,fp64", "model/gltf-binary,application/sla"},
    {"transition_voxels", true, false, false, "axis_pair_or_multi", "escape", "auto,openmp,avx2,cuda", "auto,fp32,fp64", "application/sla"},
    {"special_points_enumerate", true, false, false, "mandelbrot_special_points", "newton_residual", "openmp", "fp64,mpfr", "application/json"},
    {"special_points_search", true, false, false, "mandelbrot_special_points", "newton_residual", "openmp", "fp64,mpfr", "application/json"},
    {"special_points_auto", false, true, false, "mandelbrot_special_points", "newton_residual", "openmp", "fp64,mpfr", "application/json"},
    {"special_points_seed", false, true, false, "mandelbrot_special_points", "newton_residual", "openmp", "fp64,mpfr", "application/json"},
    {"special_points_snap", false, true, false, "mandelbrot_special_points", "newton_residual", "openmp", "fp64,mpfr", "application/json"},
    {"benchmark", true, false, false, "builtin_benchmark_workloads", "escape", "available_candidates", "available_candidates", "application/json"},
}};

const ComputeCapability* findCapability(std::string_view kind) {
    for (const auto& capability : COMPUTE_CAPABILITIES) {
        if (kind == capability.kind) return &capability;
    }
    return nullptr;
}

Json commaSeparatedArray(std::string_view values) {
    Json result = Json::array();
    std::size_t start = 0;
    while (start <= values.size()) {
        const std::size_t comma = values.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? values.size() : comma;
        if (end > start) result.push_back(values.substr(start, end - start));
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return result;
}

bool commaSeparatedContains(std::string_view values, std::string_view wanted) {
    std::size_t start = 0;
    while (start <= values.size()) {
        const std::size_t comma = values.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? values.size() : comma;
        if (values.substr(start, end - start) == wanted) return true;
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return false;
}

std::shared_ptr<std::mutex> idempotencyLock(const std::string& key) {
    std::lock_guard<std::mutex> lock(g_idempotencyMutex);
    auto& weak = g_idempotencyLocks[key];
    auto shared = weak.lock();
    if (!shared) {
        shared = std::make_shared<std::mutex>();
        weak = shared;
    }
    return shared;
}

std::string rendererVersion() {
    const char* value = std::getenv("FSD_RENDERER_VERSION");
    return value != nullptr && *value != '\0' ? value : "dev";
}

Json computeError(const std::string& code, const std::string& message,
                  Json details = Json::object()) {
    return Json{{"error", {{"code", code}, {"message", message}, {"details", std::move(details)}}}};
}

void requireComputeRun(JobRunner& runner, const std::string& runId) {
    try {
        (void)runner.getRun(runId);
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).starts_with("run not found:")) {
            throw HttpError(404, computeError(
                "COMPUTE_RUN_NOT_FOUND", "Compute run was not found",
                Json{{"computeRunId", runId}}).dump());
        }
        throw;
    }
}

[[noreturn]] void badRequest(const std::string& code, const std::string& message,
                             Json details = Json::object()) {
    throw HttpError(400, computeError(code, message, std::move(details)).dump());
}

[[noreturn]] void unsupported(const std::string& kind, const std::string& message) {
    throw HttpError(422, computeError(
        "UNSUPPORTED_CAPABILITY", message, Json{{"kind", kind}}).dump());
}

std::string requireStringField(const std::string& kind, const Json& payload,
                               const char* field) {
    if (!payload[field].is_string()) {
        badRequest("INVALID_REQUEST", std::string(field) + " must be a string",
                   Json{{"kind", kind}, {"field", field}});
    }
    return payload[field].get<std::string>();
}

void validateAdvertisedValue(const std::string& kind, const Json& payload,
                             const char* field, std::string_view allowed) {
    if (!payload.contains(field) || payload[field].is_null()) return;
    const std::string value = requireStringField(kind, payload, field);
    if (!commaSeparatedContains(allowed, value)) {
        throw HttpError(422, computeError(
            "UNSUPPORTED_CAPABILITY", std::string("unsupported ") + field,
            Json{{"kind", kind}, {"field", field}, {"value", value},
                 {"allowed", commaSeparatedArray(allowed)}}).dump());
    }
}

void validateBuiltinVariantValue(const std::string& kind, const Json& value,
                                 const char* field, bool axisOnly) {
    if (!value.is_string()) {
        badRequest("INVALID_REQUEST", std::string(field) + " must be a canonical variant string",
                   Json{{"kind", kind}, {"field", field}});
    }
    const std::string name = value.get<std::string>();
    compute::Variant variant;
    const bool known = compute::variant_from_name(name.c_str(), variant) &&
        variant != compute::Variant::Custom;
    const bool axisCompatible = known &&
        static_cast<int>(variant) <= static_cast<int>(compute::Variant::Ship);
    if (!known || (axisOnly && !axisCompatible)) {
        throw HttpError(422, computeError(
            "UNSUPPORTED_CAPABILITY",
            axisOnly ? "variant has no axis-transition lift" : "unknown built-in variant",
            Json{{"kind", kind}, {"field", field}, {"value", name}}).dump());
    }
}

void validateAxisVariants(const std::string& kind, const Json& payload) {
    for (const char* field : {"transitionFrom", "transitionTo"}) {
        if (payload.contains(field) && !payload[field].is_null()) {
            validateBuiltinVariantValue(kind, payload[field], field, true);
        }
    }
    if (payload.contains("transitionVariants") && !payload["transitionVariants"].is_null()) {
        if (!payload["transitionVariants"].is_array()) {
            badRequest("INVALID_REQUEST", "transitionVariants must be an array",
                       Json{{"kind", kind}, {"field", "transitionVariants"}});
        }
        for (const Json& value : payload["transitionVariants"]) {
            validateBuiltinVariantValue(kind, value, "transitionVariants", true);
        }
    }
    if (payload.contains("transitionLegs") && !payload["transitionLegs"].is_null()) {
        if (!payload["transitionLegs"].is_array()) {
            badRequest("INVALID_REQUEST", "transitionLegs must be an array",
                       Json{{"kind", kind}, {"field", "transitionLegs"}});
        }
        for (const Json& leg : payload["transitionLegs"]) {
            const Json* value = &leg;
            if (leg.is_object()) {
                if (!leg.contains("variant")) {
                    badRequest("INVALID_REQUEST", "transition leg requires variant",
                               Json{{"kind", kind}, {"field", "transitionLegs"}});
                }
                value = &leg["variant"];
            }
            validateBuiltinVariantValue(kind, *value, "transitionLegs.variant", true);
        }
    }
}

void validateCapabilityPayload(const std::string& kind, const Json& payload,
                               const ComputeCapability& capability) {
    validateAdvertisedValue(kind, payload, "metric", capability.metrics);
    validateAdvertisedValue(kind, payload, "engine", capability.engines);
    validateAdvertisedValue(kind, payload, "scalarType", capability.scalars);

    const std::string_view profile = capability.variantProfile;
    if (profile == "builtin_2d_or_safe_dsl") {
        if (payload.contains("variant") && !payload["variant"].is_null()) {
            validateBuiltinVariantValue(kind, payload["variant"], "variant", false);
        }
    } else if (profile == "axis_pair_or_multi") {
        validateAxisVariants(kind, payload);
    }
}

Json parseEnvelope(const std::string& body, std::string& kind,
                   std::string* idempotencyKey = nullptr) {
    const Json envelope = parseJsonBody(body);
    const int schemaVersion = envelope.value("schemaVersion", 0);
    if (schemaVersion != COMPUTE_SCHEMA_VERSION) {
        badRequest("UNSUPPORTED_SCHEMA_VERSION", "schemaVersion must be 1",
                   Json{{"received", schemaVersion}});
    }
    kind = envelope.value("kind", std::string());
    if (kind.empty()) badRequest("INVALID_REQUEST", "kind is required");
    if (!envelope.contains("payload") || !envelope["payload"].is_object()) {
        badRequest("INVALID_REQUEST", "payload must be an object");
    }
    if (envelope.contains("orbitProgram")) {
        unsupported(kind, "orbitProgram is not enabled in this Compute build");
    }
    if (idempotencyKey != nullptr) {
        *idempotencyKey = envelope.value("idempotencyKey", std::string());
        if (idempotencyKey->empty() || idempotencyKey->size() > 200) {
            badRequest("INVALID_IDEMPOTENCY_KEY", "idempotencyKey must contain 1..200 characters");
        }
    }
    return envelope["payload"];
}

Json parseLegacyResponse(const std::string& text, const std::string& kind) {
    try {
        return Json::parse(text);
    } catch (...) {
        throw HttpError(500, computeError(
            "COMPUTE_ADAPTER_ERROR", "legacy route returned invalid JSON",
            Json{{"kind", kind}}).dump());
    }
}

std::shared_ptr<const compute::OrbitProgram> validateOrbitPayload(
    const std::string& kind, const Json& payload, bool persistent) {
    const ComputeCapability* capability = findCapability(kind);
    if (capability == nullptr || (persistent ? !capability->persistent : !capability->preview)) {
        unsupported(kind, persistent ? "unknown persistent Compute kind" : "unknown preview Compute kind");
    }
    validateCapabilityPayload(kind, payload, *capability);
    if (!payload.contains("orbitProgram") || payload["orbitProgram"].is_null()) return {};
    if (!capability->orbitPayload) {
        unsupported(kind, "this Compute build supports Orbit Program only for 2D/Julia map and HS outputs");
    }
    if (payload.value("metric", std::string("escape")) != "escape") {
        const std::string metric = payload.value("metric", std::string("escape"));
        if ((kind == "map_image" || kind == "raw_field") || metric == "mandel_ship_agree") {
            unsupported(kind, "requested metric is not compatible with Orbit Program");
        }
    }
    if (payload.contains("transitionTheta") || payload.contains("transitionThetaMilliDeg") ||
        payload.contains("transitionVariants")) {
        unsupported(kind, "Orbit Program cannot be combined with axis transition");
    }
    try {
        return compute::parse_orbit_program_json(payload["orbitProgram"]);
    } catch (const compute::FormulaCompileError& error) {
        badRequest(error.code(), error.what(), Json{{"position", error.position()}});
    } catch (const std::exception& error) {
        badRequest("INVALID_ORBIT_PROGRAM", error.what());
    }
}

std::string digestHex(EVP_MD_CTX* context) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestLength = 0;
    if (EVP_DigestFinal_ex(context, digest.data(), &digestLength) != 1) {
        throw std::runtime_error("failed to finalize SHA-256");
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digestLength; ++i) {
        out << std::setw(2) << static_cast<int>(digest[i]);
    }
    return out.str();
}

std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> sha256Context() {
    EVP_MD_CTX* raw = EVP_MD_CTX_new();
    if (raw == nullptr) throw std::runtime_error("failed to allocate SHA-256 context");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(raw, EVP_MD_CTX_free);
    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("failed to initialize SHA-256");
    }
    return ctx;
}

std::string sha256File(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("failed to open artifact for hashing");
    auto ctx = sha256Context();
    std::array<char, 1024 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0 && EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<size_t>(count)) != 1) {
            throw std::runtime_error("failed to update SHA-256");
        }
    }
    if (!input.eof()) throw std::runtime_error("failed while hashing artifact");
    return digestHex(ctx.get());
}

std::string sha256Text(const std::string& value) {
    auto ctx = sha256Context();
    if (EVP_DigestUpdate(ctx.get(), value.data(), value.size()) != 1) {
        throw std::runtime_error("failed to calculate recipe SHA-256");
    }
    return digestHex(ctx.get());
}

Json normalizedRunResponse(const std::string& kind, const Json& legacy) {
    const std::string runId = legacy.value("runId", std::string());
    if (runId.empty()) {
        throw HttpError(500, computeError(
            "COMPUTE_ADAPTER_ERROR", "persistent route did not return runId",
            Json{{"kind", kind}}).dump());
    }
    Json data = {
        {"computeRunId", runId},
        {"kind", kind},
        {"status", legacy.value("status", std::string("completed"))},
        {"legacyResult", legacy},
    };
    if (legacy.contains("effective")) data["effective"] = legacy["effective"];
    return Json{{"schemaVersion", COMPUTE_SCHEMA_VERSION}, {"data", std::move(data)}};
}

Json hardwareCapabilitiesJson() {
    const auto caps = compute::runtime_capabilities();
    return {
        {"cpu", {
            {"logicalCores", caps.logical_cores},
            {"physicalCores", caps.physical_cores},
            {"openmp", {{"compiled", caps.openmp_compiled}, {"runtime", caps.openmp_runtime}}},
            {"avx2", {{"compiled", caps.avx2_compiled}, {"runtime", caps.avx2_runtime}}},
            {"avx512", {{"compiled", caps.avx512_compiled}, {"runtime", caps.avx512_runtime}}},
        }},
        {"cuda", {
            {"compiled", caps.cuda_compiled}, {"runtime", caps.cuda_runtime},
            {"deviceCount", caps.cuda_device_count}, {"name", caps.cuda_name},
            {"computeCapability", {
                {"major", caps.cuda_compute_major}, {"minor", caps.cuda_compute_minor},
            }},
            {"totalVramBytes", caps.cuda_total_vram},
            {"freeVramBytes", caps.cuda_free_vram},
        }},
    };
}

std::string hardwareClassForEngine(const std::string& engine) {
    const bool cuda = engine.find("cuda") != std::string::npos;
    const bool cpu = engine.find("openmp") != std::string::npos ||
        engine.find("avx2") != std::string::npos ||
        engine.find("avx512") != std::string::npos ||
        engine.find("perturbation") != std::string::npos;
    if (cuda && cpu) return "hybrid";
    if (cuda) return "gpu";
    if (cpu) return "cpu";
    return "unknown";
}

bool requestedEngineMatches(const std::string& requested, const std::string& actual) {
    if (requested.empty() || requested == "auto" || actual.empty()) return true;
    return actual.find(requested) != std::string::npos;
}

Json hardwareExecutionJson(const std::string& status, const Json& progress,
                           const Json& params) {
    const Json details = progress.value("details", Json::object());
    if (details.contains("hardwareExecutions") && details["hardwareExecutions"].is_array()) {
        return {
            {"mode", "multi_path"},
            {"kernelReported", status == "completed" && progress.value("kernelReported", false)},
            {"evidenceSource", "benchmark_candidate_telemetry"},
            {"paths", details["hardwareExecutions"]},
            {"elapsedMs", progress.value("elapsedMs", 0.0)},
        };
    }
    const std::string requestedEngine = params.value("engine", std::string("auto"));
    const std::string requestedScalar = params.value("scalarType", std::string("auto"));
    const std::string actualEngine = progress.value(
        "engine", progress.value("finalFrameEngine", std::string()));
    const std::string actualScalar = progress.value(
        "scalar", progress.value("finalFrameScalar", std::string()));
    const std::string hardwareClass = hardwareClassForEngine(actualEngine);
    const bool completed = status == "completed";
    const bool kernelReported = completed &&
        progress.value("kernelReported", false) && !actualEngine.empty();
    const bool engineFallback = kernelReported &&
        !requestedEngineMatches(requestedEngine, actualEngine);
    const auto caps = compute::runtime_capabilities();
    bool runtimeAvailable = false;
    if (hardwareClass == "gpu") runtimeAvailable = caps.cuda_runtime;
    else if (hardwareClass == "hybrid") runtimeAvailable = caps.cuda_runtime && caps.openmp_runtime;
    else if (hardwareClass == "cpu") runtimeAvailable = caps.openmp_runtime;

    Json evidence = {
        {"requestedEngine", requestedEngine}, {"requestedScalar", requestedScalar},
        {"actualEngine", actualEngine}, {"actualScalar", actualScalar},
        {"hardwareClass", hardwareClass}, {"kernelReported", kernelReported},
        {"runtimeAvailable", runtimeAvailable}, {"engineFallback", engineFallback},
        {"evidenceSource", kernelReported ? "kernel_completion_telemetry" : "not_yet_reported"},
        {"elapsedMs", progress.value("elapsedMs", 0.0)},
    };
    if (engineFallback) {
        evidence["fallbackReason"] = "requested engine was not reported by the completed kernel";
    } else {
        evidence["fallbackReason"] = nullptr;
    }
    return evidence;
}

Json runPayload(Json payload, const std::string& kind,
                const std::filesystem::path& repoRoot, JobRunner& runner) {
    if (kind == "map_image") {
        payload["taskType"] = "still_export";
        payload["background"] = true;
        return parseLegacyResponse(mapRenderRoute(repoRoot, runner, payload.dump()), kind);
    }
    if (kind == "ln_map") {
        payload["background"] = true;
        return parseLegacyResponse(lnMapRenderRoute(repoRoot, runner, payload.dump()), kind);
    }
    if (kind == "zoom_video") {
        payload["background"] = true;
        return parseLegacyResponse(videoExportRoute(repoRoot, runner, payload.dump()), kind);
    }
    if (kind == "legacy_zoom_video") {
        payload["background"] = true;
        return parseLegacyResponse(zoomVideoRoute(repoRoot, runner, payload.dump()), kind);
    }
    if (kind == "transition_video") {
        payload["background"] = true;
        return parseLegacyResponse(transitionVideoExportRoute(repoRoot, runner, payload.dump()), kind);
    }
    if (kind == "hs_mesh") {
        payload["background"] = true;
        return parseLegacyResponse(hsMeshRoute(repoRoot, runner, payload.dump()), kind);
    }
    if (kind == "hs_field") {
        payload["background"] = true;
        return parseLegacyResponse(hsFieldRoute(repoRoot, runner, payload.dump()), kind);
    }
    if (kind == "transition_mesh") {
        payload["background"] = true;
        return parseLegacyResponse(transitionMeshRoute(repoRoot, runner, payload.dump()), kind);
    }
    if (kind == "transition_voxels") {
        payload["background"] = true;
        return parseLegacyResponse(transitionVoxelsRoute(repoRoot, runner, payload.dump()), kind);
    }
    if (kind == "special_points_enumerate") {
        payload["background"] = true;
        return parseLegacyResponse(specialPointsEnumerateRoute(repoRoot, runner, payload.dump()), kind);
    }
    if (kind == "special_points_search") {
        return parseLegacyResponse(specialPointsSearchRoute(repoRoot, runner, payload.dump()), kind);
    }
    if (kind == "benchmark") {
        payload["background"] = true;
        return parseLegacyResponse(benchmarkRoute(runner, payload.dump()), kind);
    }
    unsupported(kind, "unknown persistent Compute job kind");
}

} // namespace

std::string computeV1HealthRoute() {
    return Json{
        {"schemaVersion", COMPUTE_SCHEMA_VERSION},
        {"status", "ok"},
        {"service", "fractal-studio-compute"},
        {"rendererVersion", rendererVersion()},
    }.dump();
}

std::string computeV1CapabilitiesRoute() {
    Json persistentKinds = Json::array();
    Json previewKinds = Json::array();
    Json jobs = Json::array();
    for (const auto& capability : COMPUTE_CAPABILITIES) {
        if (capability.persistent) persistentKinds.push_back(capability.kind);
        if (capability.preview) previewKinds.push_back(capability.kind);
        jobs.push_back({
            {"kind", capability.kind}, {"persistent", capability.persistent},
            {"preview", capability.preview}, {"orbitProgram", capability.orbitPayload},
            {"variantProfile", capability.variantProfile},
            {"metrics", commaSeparatedArray(capability.metrics)},
            {"engines", commaSeparatedArray(capability.engines)},
            {"scalars", commaSeparatedArray(capability.scalars)},
            {"outputMediaTypes", commaSeparatedArray(capability.outputMediaTypes)},
        });
    }
    return Json{
        {"schemaVersion", COMPUTE_SCHEMA_VERSION},
        {"rendererVersion", rendererVersion()},
        {"persistentKinds", std::move(persistentKinds)},
        {"previewKinds", std::move(previewKinds)},
        {"jobs", std::move(jobs)},
        {"orbitPrograms", {
            {"formula", true}, {"sequence", true}, {"weightedSchedule", false},
            {"outputBlend", false}, {"axisTransition", true}, {"axisMulti", true},
        }},
        {"orbitCompatibility", {
            {"mapImage", true}, {"rawField", true}, {"julia", true},
            {"lnMap", true}, {"hsField", true}, {"hsMesh", true},
            {"zoomVideo", true}, {"transitionVideo", false},
            {"transitionMesh", false}, {"transitionVoxels", false},
        }},
        {"customFormula", {
            {"legacyNativeCompile", legacyFormulaCompilerEnabled()},
            {"safeDsl", true},
        }},
        {"escapeSemantics", {{"certifiedRadius", true}, {"strictUnverified", true}}},
        {"hardware", hardwareCapabilitiesJson()},
    }.dump();
}

void computeV1ValidateOrbitRequest(const std::string& body, bool persistent) {
    std::string kind;
    const Json payload = parseEnvelope(body, kind);
    (void)validateOrbitPayload(kind, payload, persistent);
}

std::string computeV1PreviewJsonRoute(const std::filesystem::path& repoRoot,
                                      JobRunner& runner,
                                      const std::string& body) {
    std::string kind;
    const Json payload = parseEnvelope(body, kind);
    (void)validateOrbitPayload(kind, payload, false);
    Json legacy;
    if (kind == "raw_field") legacy = parseLegacyResponse(mapFieldRoute(repoRoot, payload.dump()), kind);
    else if (kind == "video_preview") legacy = parseLegacyResponse(videoPreviewRoute(repoRoot, runner, payload.dump()), kind);
    else if (kind == "transition_video_preview") legacy = parseLegacyResponse(transitionVideoPreviewRoute(repoRoot, runner, payload.dump()), kind);
    else if (kind == "special_points_auto") legacy = parseLegacyResponse(specialPointsAutoRoute(repoRoot, payload.dump()), kind);
    else if (kind == "special_points_seed") legacy = parseLegacyResponse(specialPointsSeedRoute(repoRoot, payload.dump()), kind);
    else if (kind == "special_points_snap") legacy = parseLegacyResponse(specialPointsSnapRoute(payload.dump()), kind);
    else if (kind == "map_image") unsupported(kind, "map_image preview uses the binary HTTP adapter");
    else unsupported(kind, "unknown preview Compute kind");
    return Json{{"schemaVersion", COMPUTE_SCHEMA_VERSION}, {"data", std::move(legacy)}}.dump();
}

std::string computeV1CreateRunRoute(const std::filesystem::path& repoRoot,
                                    JobRunner& runner,
                                    const std::string& body) {
    std::string kind;
    std::string idempotencyKey;
    Json payload = parseEnvelope(body, kind, &idempotencyKey);
    (void)validateOrbitPayload(kind, payload, true);
    const auto requestLock = idempotencyLock(idempotencyKey);
    std::lock_guard<std::mutex> lock(*requestLock);
    Db db = openDb(repoRoot);
    const std::string requestHash = sha256Text(kind + "\n" + payload.dump());
    std::string cachedHash;
    std::string cached;
    if (db.getComputeRequestResponse(idempotencyKey, cachedHash, cached)) {
        if (!cachedHash.empty() && cachedHash != requestHash) {
            throw HttpError(409, computeError(
                "IDEMPOTENCY_CONFLICT",
                "idempotencyKey was already used for a different Compute request",
                Json{{"idempotencyKey", idempotencyKey}}).dump());
        }
        return cached;
    }
    const std::string response = normalizedRunResponse(
        kind, runPayload(std::move(payload), kind, repoRoot, runner)).dump();
    db.upsertComputeRequestResponse(idempotencyKey, requestHash, response, nowUnixMs());
    return response;
}

std::string computeV1RunStatusRoute(const std::filesystem::path& repoRoot,
                                    JobRunner& runner,
                                    const std::string& runId) {
    requireComputeRun(runner, runId);
    const Json legacy = parseLegacyResponse(
        runStatusRoute(repoRoot, runner, "runId=" + runId), "run_status");
    Json params = Json::object();
    try {
        params = Json::parse(openDb(repoRoot).getRun(runId).paramsJson);
    } catch (...) {}
    const Json progress = legacy.value("progress", Json::object());
    Json artifacts = Json::array();
    for (const Json& artifact : legacy.value("artifacts", Json::array())) {
        artifacts.push_back({
            {"artifactId", artifact.value("artifactId", std::string())},
            {"name", artifact.value("name", std::string())},
            {"kind", artifact.value("kind", std::string())},
        });
    }
    return Json{{"schemaVersion", COMPUTE_SCHEMA_VERSION}, {"data", {
        {"computeRunId", runId}, {"status", legacy.value("status", std::string())},
        {"module", legacy.value("module", std::string())},
        {"startedAt", legacy.value("startedAt", 0LL)},
        {"finishedAt", legacy.value("finishedAt", 0LL)},
        {"cancelRequested", legacy.value("cancelRequested", false)},
        {"progress", progress},
        {"hardwareExecution", hardwareExecutionJson(
            legacy.value("status", std::string()), progress, params)},
        {"artifacts", artifacts},
    }}}.dump();
}

std::string computeV1CancelRunRoute(JobRunner& runner,
                                   const std::string& runId,
                                   const std::string& body) {
    requireComputeRun(runner, runId);
    const Json legacy = parseLegacyResponse(cancelRunRoute(runner, runId, body), "cancel");
    return Json{{"schemaVersion", COMPUTE_SCHEMA_VERSION}, {"data", {
        {"computeRunId", runId}, {"status", legacy.value("status", std::string())},
        {"accepted", legacy.value("accepted", false)},
        {"cancelRequested", legacy.value("cancelRequested", false)},
    }}}.dump();
}

std::string computeV1ManifestRoute(const std::filesystem::path& repoRoot,
                                   JobRunner& runner,
                                   const std::string& runId) {
    requireComputeRun(runner, runId);
    const Json status = parseLegacyResponse(
        runStatusRoute(repoRoot, runner, "runId=" + runId), "manifest");
    Db db = openDb(repoRoot);
    const RunRow row = db.getRun(runId);
    const Json listed = parseLegacyResponse(
        artifactsListRoute(repoRoot, "runId=" + runId), "manifest_artifacts");
    Json artifacts = Json::array();
    for (const Json& item : listed.value("items", Json::array())) {
        const std::string artifactId = item.value("artifactId", std::string());
        const ArtifactFile file = artifactFileRoute(repoRoot, "artifactId=" + artifactId);
        artifacts.push_back({
            {"artifactId", artifactId}, {"name", item.value("name", std::string())},
            {"kind", item.value("kind", std::string())}, {"mediaType", file.contentType},
            {"sizeBytes", file.sizeBytes}, {"sha256", sha256File(file.path)},
            {"contentPath", "/compute/v1/artifacts?artifactId=" + artifactId},
        });
    }
    const Json progress = status.value("progress", Json::object());
    const std::string engine = progress.value("engine", progress.value("finalFrameEngine", std::string()));
    const std::string scalar = progress.value("scalar", progress.value("finalFrameScalar", std::string()));
    Json escapeAnalysis = {
        {"status", "unverified"}, {"certifiedRadius", nullptr},
        {"reason", "request has no analyzed Orbit Program"},
    };
    try {
        const Json params = Json::parse(row.paramsJson);
        if (params.contains("orbitProgram") && !params["orbitProgram"].is_null()) {
            const auto program = compute::parse_orbit_program_json(params["orbitProgram"]);
            escapeAnalysis = compute::escape_analysis_json(program->escape_analysis());
        } else {
            compute::Variant variant;
            const std::string name = params.value("variant", std::string("mandelbrot"));
            if (compute::variant_from_name(name.c_str(), variant)) {
                escapeAnalysis = compute::escape_analysis_json(
                    compute::OrbitProgram::builtin(variant)->escape_analysis());
            }
        }
    } catch (const std::exception& error) {
        escapeAnalysis = {
            {"status", "unverified"}, {"certifiedRadius", nullptr},
            {"reason", std::string("escape analysis could not be reconstructed: ") + error.what()},
        };
    }
    return Json{
        {"schemaVersion", COMPUTE_SCHEMA_VERSION}, {"computeRunId", runId},
        {"rendererVersion", rendererVersion()}, {"recipeHash", sha256Text(row.paramsJson)},
        {"status", status.value("status", std::string())},
        {"effective", {{"engine", engine}, {"scalar", scalar}}},
        {"hardwareExecution", hardwareExecutionJson(
            status.value("status", std::string()), progress,
            Json::parse(row.paramsJson))},
        {"escapeAnalysis", std::move(escapeAnalysis)},
        {"artifacts", artifacts},
    }.dump();
}

} // namespace fsd
