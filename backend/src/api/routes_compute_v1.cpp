#include "routes.hpp"
#include "routes_common.hpp"

#include "../include/db.hpp"
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

namespace fsd {
namespace {

constexpr int COMPUTE_SCHEMA_VERSION = 1;
std::mutex g_idempotencyMutex;
std::map<std::string, std::weak_ptr<std::mutex>> g_idempotencyLocks;

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

[[noreturn]] void badRequest(const std::string& code, const std::string& message,
                             Json details = Json::object()) {
    throw HttpError(400, computeError(code, message, std::move(details)).dump());
}

[[noreturn]] void unsupported(const std::string& kind, const std::string& message) {
    throw HttpError(422, computeError(
        "UNSUPPORTED_CAPABILITY", message, Json{{"kind", kind}}).dump());
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
    if (!payload.contains("orbitProgram") || payload["orbitProgram"].is_null()) return {};
    const bool supportedKind = kind == "map_image" || (!persistent && kind == "raw_field");
    if (!supportedKind) {
        unsupported(kind, "this Compute build supports Orbit Program only for 2D/Julia map_image and raw_field");
    }
    if (payload.value("metric", std::string("escape")) != "escape") {
        unsupported(kind, "Orbit Program v1 supports only metric=escape");
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

Json runPayload(Json payload, const std::string& kind,
                const std::filesystem::path& repoRoot, JobRunner& runner) {
    if (kind == "map_image") {
        payload["taskType"] = "still_export";
        payload["background"] = true;
        return parseLegacyResponse(mapRenderRoute(repoRoot, runner, payload.dump()), kind);
    }
    if (kind == "ln_map") return parseLegacyResponse(lnMapRenderRoute(repoRoot, runner, payload.dump()), kind);
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
    if (kind == "hs_mesh") return parseLegacyResponse(hsMeshRoute(repoRoot, runner, payload.dump()), kind);
    if (kind == "hs_field") return parseLegacyResponse(hsFieldRoute(repoRoot, runner, payload.dump()), kind);
    if (kind == "transition_mesh") return parseLegacyResponse(transitionMeshRoute(repoRoot, runner, payload.dump()), kind);
    if (kind == "transition_voxels") return parseLegacyResponse(transitionVoxelsRoute(repoRoot, runner, payload.dump()), kind);
    if (kind == "special_points_enumerate") {
        return parseLegacyResponse(specialPointsEnumerateRoute(repoRoot, runner, payload.dump()), kind);
    }
    if (kind == "special_points_search") {
        return parseLegacyResponse(specialPointsSearchRoute(repoRoot, runner, payload.dump()), kind);
    }
    if (kind == "benchmark") return parseLegacyResponse(benchmarkRoute(runner, payload.dump()), kind);
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
    return Json{
        {"schemaVersion", COMPUTE_SCHEMA_VERSION},
        {"rendererVersion", rendererVersion()},
        {"persistentKinds", Json::array({
            "map_image", "ln_map", "zoom_video", "legacy_zoom_video", "transition_video",
            "hs_mesh", "hs_field", "transition_mesh", "transition_voxels",
            "special_points_enumerate", "special_points_search", "benchmark",
        })},
        {"previewKinds", Json::array({
            "map_image", "raw_field", "video_preview", "transition_video_preview",
            "special_points_auto", "special_points_seed", "special_points_snap",
        })},
        {"orbitPrograms", {
            {"formula", true}, {"sequence", true}, {"weightedSchedule", false},
            {"outputBlend", false}, {"axisTransition", true}, {"axisMulti", true},
        }},
        {"customFormula", {
            {"legacyNativeCompile", legacyFormulaCompilerEnabled()},
            {"safeDsl", true},
        }},
        {"escapeSemantics", {{"certifiedRadius", true}, {"strictUnverified", true}}},
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
    std::string cached;
    if (db.getComputeRequestResponse(idempotencyKey, cached)) return cached;
    const std::string response = normalizedRunResponse(
        kind, runPayload(std::move(payload), kind, repoRoot, runner)).dump();
    db.upsertComputeRequestResponse(idempotencyKey, response, nowUnixMs());
    return response;
}

std::string computeV1RunStatusRoute(const std::filesystem::path& repoRoot,
                                    JobRunner& runner,
                                    const std::string& runId) {
    const Json legacy = parseLegacyResponse(
        runStatusRoute(repoRoot, runner, "runId=" + runId), "run_status");
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
        {"progress", legacy.value("progress", Json::object())}, {"artifacts", artifacts},
    }}}.dump();
}

std::string computeV1CancelRunRoute(JobRunner& runner,
                                   const std::string& runId,
                                   const std::string& body) {
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
        {"escapeAnalysis", std::move(escapeAnalysis)},
        {"artifacts", artifacts},
    }.dump();
}

} // namespace fsd
