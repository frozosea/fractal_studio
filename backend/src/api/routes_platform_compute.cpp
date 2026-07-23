#include "routes.hpp"
#include "routes_common.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fsd {
namespace {

constexpr std::size_t MAX_PLATFORM_BODY_BYTES = 1024 * 1024;
constexpr std::uintmax_t MAX_PLATFORM_ARTIFACT_BYTES = 524288000;

class ContractError final : public std::runtime_error {
public:
    ContractError(int status, std::string code, std::string message)
        : std::runtime_error(message), status_(status), code_(std::move(code)) {}
    int status() const noexcept { return status_; }
    const std::string& code() const noexcept { return code_; }

private:
    int status_;
    std::string code_;
};

[[noreturn]] void reject(int status, const std::string& code, const std::string& message) {
    throw ContractError(status, code, message);
}

bool isUuid(const std::string& value) {
    static const std::regex uuid(
        "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-5][0-9a-fA-F]{3}-"
        "[89aAbB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$");
    return std::regex_match(value, uuid);
}

Json strictObject(const std::string& body) {
    if (body.size() > MAX_PLATFORM_BODY_BYTES) {
        reject(413, "request_too_large", "request body exceeds the Compute contract limit");
    }
    Json value;
    try {
        value = Json::parse(body);
    } catch (...) {
        reject(400, "invalid_json", "request body must be valid JSON");
    }
    if (!value.is_object()) reject(400, "invalid_request", "request body must be a JSON object");
    return value;
}

const Json& required(const Json& value, const char* field) {
    if (!value.contains(field) || value[field].is_null()) {
        reject(400, "invalid_request", std::string(field) + " is required");
    }
    return value[field];
}

std::string requireString(const Json& value, const char* field, std::size_t maxLength) {
    const Json& item = required(value, field);
    if (!item.is_string()) reject(400, "invalid_request", std::string(field) + " must be a string");
    const std::string result = item.get<std::string>();
    if (result.empty()) reject(400, "invalid_request", std::string(field) + " must not be empty");
    if (result.size() > maxLength) {
        reject(413, "request_limit_exceeded", std::string(field) + " exceeds the maximum length");
    }
    return result;
}

std::string optionalString(const Json& value, const char* field, std::size_t maxLength) {
    if (!value.contains(field) || value[field].is_null()) return {};
    if (!value[field].is_string()) {
        reject(400, "invalid_request", std::string(field) + " must be a string");
    }
    const std::string result = value[field].get<std::string>();
    if (result.size() > maxLength) {
        reject(413, "request_limit_exceeded", std::string(field) + " exceeds the maximum length");
    }
    return result;
}

double requireFiniteNumber(const Json& value, const char* field) {
    const Json& item = required(value, field);
    if (!item.is_number()) reject(400, "invalid_request", std::string(field) + " must be a number");
    const double result = item.get<double>();
    if (!std::isfinite(result)) reject(400, "invalid_request", std::string(field) + " must be finite");
    return result;
}

double optionalFiniteNumber(const Json& value, const char* field, double fallback) {
    if (!value.contains(field) || value[field].is_null()) return fallback;
    if (!value[field].is_number()) {
        reject(400, "invalid_request", std::string(field) + " must be a number");
    }
    const double result = value[field].get<double>();
    if (!std::isfinite(result)) reject(400, "invalid_request", std::string(field) + " must be finite");
    return result;
}

int requireBoundedInteger(const Json& value, const char* field, int minimum, int maximum) {
    const Json& item = required(value, field);
    if (!item.is_number_integer()) {
        reject(400, "invalid_request", std::string(field) + " must be an integer");
    }
    long long result = 0;
    try {
        result = item.get<long long>();
    } catch (...) {
        reject(413, "request_limit_exceeded", std::string(field) + " exceeds the supported range");
    }
    if (result < minimum) reject(400, "invalid_request", std::string(field) + " is below the minimum");
    if (result > maximum) reject(413, "request_limit_exceeded", std::string(field) + " exceeds the maximum");
    return static_cast<int>(result);
}

void optionalPositive(const Json& value, const char* field) {
    if (!value.contains(field) || value[field].is_null()) return;
    if (!(optionalFiniteNumber(value, field, 0.0) > 0.0)) {
        reject(400, "invalid_request", std::string(field) + " must be greater than zero");
    }
}

void optionalBoolean(const Json& value, const char* field) {
    if (value.contains(field) && !value[field].is_null() && !value[field].is_boolean()) {
        reject(400, "invalid_request", std::string(field) + " must be a boolean");
    }
}

std::string requireClientJobId(const Json& value) {
    const std::string id = requireString(value, "clientJobId", 36);
    if (!isUuid(id)) reject(400, "invalid_client_job_id", "clientJobId must be a UUID");
    return id;
}

void validateOptionalRequestId(const Json& value) {
    if (!value.contains("requestId") || value["requestId"].is_null()) return;
    if (!value["requestId"].is_string() || !isUuid(value["requestId"].get<std::string>())) {
        reject(400, "invalid_request_id", "requestId must be a UUID");
    }
}

std::string mapEngine(const Json& value) {
    const std::string engine = optionalString(value, "engine", 16);
    if (engine.empty() || engine == "auto") return "auto";
    if (engine == "cpu") return "openmp";
    if (engine == "cuda") return "cuda";
    reject(400, "invalid_request", "engine must be auto, cpu, or cuda");
}

std::string mapMapScalar(const Json& value) {
    const std::string scalar = optionalString(value, "scalarType", 24);
    if (scalar.empty() || scalar == "auto") return "auto";
    if (scalar == "float") return "fp32";
    if (scalar == "double") return "fp64";
    if (scalar == "long_double") return "fp80";
    reject(400, "invalid_request", "scalarType must be auto, float, double, or long_double");
}

Json validateMap(Json value, int maximumDimension) {
    (void)requireBoundedInteger(value, "width", 1, maximumDimension);
    (void)requireBoundedInteger(value, "height", 1, maximumDimension);
    (void)requireBoundedInteger(value, "iterations", 1, 1000000);
    (void)requireString(value, "variant", 64);
    (void)requireFiniteNumber(value, "centerRe");
    (void)requireFiniteNumber(value, "centerIm");
    if (!(requireFiniteNumber(value, "scale") > 0.0)) {
        reject(400, "invalid_request", "scale must be greater than zero");
    }
    (void)optionalString(value, "colorMap", 64);
    optionalBoolean(value, "julia");
    if (value.contains("juliaRe") && !value["juliaRe"].is_null())
        (void)optionalFiniteNumber(value, "juliaRe", 0.0);
    if (value.contains("juliaIm") && !value["juliaIm"].is_null())
        (void)optionalFiniteNumber(value, "juliaIm", 0.0);
    optionalPositive(value, "bailout");
    validateOptionalRequestId(value);
    value["engine"] = mapEngine(value);
    value["scalarType"] = mapMapScalar(value);
    return value;
}

Json validateHsMesh(Json value) {
    (void)requireFiniteNumber(value, "centerRe");
    (void)requireFiniteNumber(value, "centerIm");
    if (!(requireFiniteNumber(value, "scale") > 0.0))
        reject(400, "invalid_request", "scale must be greater than zero");
    (void)requireBoundedInteger(value, "resolution", 8, 1024);
    (void)requireBoundedInteger(value, "iterations", 1, 1000000);
    (void)requireString(value, "variant", 64);
    if (value.contains("heightScale") && !value["heightScale"].is_null())
        (void)optionalFiniteNumber(value, "heightScale", 0.0);
    optionalPositive(value, "heightClamp");
    optionalPositive(value, "bailout");
    return value;
}

Json validateTransitionMesh(Json value) {
    for (const char* field : {"centerX", "centerY", "centerZ"})
        (void)requireFiniteNumber(value, field);
    if (!(requireFiniteNumber(value, "extent") > 0.0))
        reject(400, "invalid_request", "extent must be greater than zero");
    (void)requireBoundedInteger(value, "resolution", 8, 1024);
    (void)requireBoundedInteger(value, "iterations", 1, 10000);
    (void)requireString(value, "transitionFrom", 64);
    (void)requireString(value, "transitionTo", 64);
    optionalPositive(value, "bailout");
    if (value.contains("iso") && !value["iso"].is_null())
        (void)optionalFiniteNumber(value, "iso", 0.0);
    value["engine"] = mapEngine(value);
    const std::string scalar = optionalString(value, "scalarType", 16);
    if (!scalar.empty() && scalar != "fp32" && scalar != "fp64")
        reject(400, "invalid_request", "scalarType must be fp32 or fp64");
    if (scalar.empty()) value["scalarType"] = "fp32";
    return value;
}

Json parseJsonResponse(const std::string& text) {
    try {
        return Json::parse(text);
    } catch (...) {
        throw std::runtime_error("internal Compute adapter returned invalid JSON");
    }
}

std::string computeEnvelope(const std::string& kind, const std::string& clientJobId,
                            Json payload) {
    return Json{
        {"schemaVersion", 1},
        {"kind", kind},
        {"idempotencyKey", clientJobId},
        {"payload", std::move(payload)},
    }.dump();
}

PlatformComputeResponse createRun(const std::filesystem::path& repoRoot,
                                  JobRunner& runner, const std::string& kind,
                                  Json payload, const std::string& clientJobId) {
    payload["clientJobId"] = clientJobId;
    const Json internal = parseJsonResponse(computeV1CreateRunRoute(
        repoRoot, runner, computeEnvelope(kind, clientJobId, std::move(payload))));
    const Json& data = internal.at("data");
    return {200, Json{
        {"runId", data.at("computeRunId")},
        {"clientJobId", clientJobId},
        {"status", data.value("status", std::string("queued"))},
    }.dump(), "application/json", {}};
}

std::pair<RunRow, Json> platformRun(const std::filesystem::path& repoRoot,
                                    const std::string& runId) {
    if (runId.empty() || runId.size() > 128) reject(400, "invalid_request", "runId must contain 1..128 characters");
    try {
        validateRunIdForPath(runId);
    } catch (...) {
        reject(400, "invalid_request", "runId contains invalid path characters");
    }
    RunRow row;
    try {
        row = openDb(repoRoot).getRun(runId);
    } catch (...) {
        reject(404, "run_not_found", "Compute run was not found");
    }
    Json params;
    try {
        params = Json::parse(row.paramsJson);
    } catch (...) {
        reject(500, "compute_failure", "Compute run metadata is invalid");
    }
    const std::string clientJobId = params.value("clientJobId", std::string());
    if (!isUuid(clientJobId)) reject(404, "run_not_found", "Compute run was not found");
    return {std::move(row), std::move(params)};
}

int progressPercent(const std::string& status, const Json& progress) {
    if (status == "completed") return 100;
    if (status == "queued") return 0;
    double percent = progress.value("percent", 0.0);
    if (!std::isfinite(percent)) percent = 0.0;
    return static_cast<int>(std::lround(std::clamp(percent, 0.0, 100.0)));
}

Json publicArtifacts(const std::filesystem::path& repoRoot, JobRunner& runner,
                     const std::string& runId, const std::string& status) {
    Json result = Json::array();
    if (status != "completed") return result;
    const Json manifest = parseJsonResponse(computeV1ManifestRoute(repoRoot, runner, runId));
    const Json manifestArtifacts = manifest.value("artifacts", Json::array());
    const bool hasVideo = std::any_of(manifestArtifacts.begin(), manifestArtifacts.end(),
        [](const Json& item) {
            return item.value("mediaType", std::string()) == "video/mp4";
        });
    for (const Json& artifact : manifestArtifacts) {
        std::string mediaType = artifact.value("mediaType", std::string());
        if (mediaType == "application/sla") mediaType = "model/stl";
        if (mediaType != "image/png" && mediaType != "video/mp4" &&
            mediaType != "model/gltf-binary" && mediaType != "model/stl") continue;
        const std::uintmax_t size = artifact.value("sizeBytes", std::uintmax_t{0});
        if (size == 0 || size > MAX_PLATFORM_ARTIFACT_BYTES) continue;
        const bool previewSource = hasVideo && mediaType == "image/png";
        result.push_back({
            {"artifactId", artifact.value("artifactId", std::string())},
            {"purpose", previewSource ? "preview_source" : "master"},
            {"mediaType", mediaType},
            {"sizeBytes", size},
        });
    }
    return result;
}

PlatformComputeResponse runStatus(const std::filesystem::path& repoRoot,
                                  JobRunner& runner, const std::string& runId) {
    const auto [row, params] = platformRun(repoRoot, runId);
    const Json internal = parseJsonResponse(computeV1RunStatusRoute(repoRoot, runner, runId));
    const Json& data = internal.at("data");
    const std::string status = data.value("status", row.status);
    const Json progress = data.value("progress", Json::object());
    Json response = {
        {"runId", runId},
        {"clientJobId", params.at("clientJobId")},
        {"status", status},
        {"progressPercent", progressPercent(status, progress)},
        {"artifacts", publicArtifacts(repoRoot, runner, runId, status)},
    };
    if (status == "failed") response["error"] = "compute_failed";
    return {200, response.dump(), "application/json", {}};
}

std::string mappedInternalErrorCode(int status, const std::string& body) {
    try {
        const Json parsed = Json::parse(body);
        const std::string internal = parsed.at("error").value("code", std::string());
        if (internal == "IDEMPOTENCY_CONFLICT") return "idempotency_conflict";
        if (internal == "UNSUPPORTED_CAPABILITY") return "unsupported_capability";
        if (internal == "COMPUTE_RUN_NOT_FOUND") return "run_not_found";
    } catch (...) {}
    if (status == 400 || status == 422) return "invalid_request";
    if (status == 404) return "run_not_found";
    if (status == 409) return "compute_conflict";
    if (status == 413) return "request_limit_exceeded";
    if (status == 429) return "compute_rate_limited";
    return "compute_failure";
}

std::string safeInternalMessage(int status, const std::string& code) {
    if (code == "idempotency_conflict") return "clientJobId was already used for a different request";
    if (code == "unsupported_capability") return "requested Compute capability is not supported";
    if (code == "run_not_found") return "Compute run was not found";
    if (status == 400 || status == 422) return "request was rejected by Compute validation";
    if (status == 409) return "Compute request conflicts with current state";
    if (status == 413) return "request exceeds the Compute contract limit";
    if (status == 429) return "Compute rate limit exceeded";
    return "Compute request failed";
}

PlatformComputeResponse routeImpl(const std::filesystem::path& repoRoot,
                                  JobRunner& runner, const std::string& method,
                                  const std::string& path, const std::string& query,
                                  const std::string& body) {
    if (path == "/api/map/render-inline") {
        if (method != "POST") reject(405, "method_not_allowed", "method not allowed");
        Json payload = validateMap(strictObject(body), 1024);
        int status = 200;
        std::string contentType;
        std::string extraHeaders;
        const std::string pixels = mapRenderInlineRoute(
            repoRoot, payload.dump(), status, contentType, extraHeaders);
        return {status, pixels, contentType, extraHeaders};
    }
    if (path == "/api/map/render") {
        if (method != "POST") reject(405, "method_not_allowed", "method not allowed");
        Json payload = strictObject(body);
        const std::string clientJobId = requireClientJobId(payload);
        if (!required(payload, "stillExport").is_boolean() || !payload["stillExport"].get<bool>())
            reject(400, "invalid_request", "stillExport must be true");
        if (!required(payload, "background").is_boolean() || !payload["background"].get<bool>())
            reject(400, "invalid_request", "background must be true");
        payload = validateMap(std::move(payload), 4096);
        return createRun(repoRoot, runner, "map_image", std::move(payload), clientJobId);
    }
    if (path == "/api/video/export") {
        if (method != "POST") reject(405, "method_not_allowed", "method not allowed");
        Json payload = strictObject(body);
        const std::string clientJobId = requireClientJobId(payload);
        payload = validateMap(std::move(payload), 4096);
        if (payload["width"].get<int>() < 128 || payload["height"].get<int>() < 128)
            reject(400, "invalid_request", "video width and height must be at least 128");
        (void)requireBoundedInteger(payload, "fps", 1, 60);
        const double duration = requireFiniteNumber(payload, "durationSeconds");
        if (!(duration > 0.0)) reject(400, "invalid_request", "durationSeconds must be greater than zero");
        if (duration > 30.0) reject(413, "request_limit_exceeded", "durationSeconds exceeds the maximum");
        payload["durationSec"] = duration;
        payload.erase("durationSeconds");
        return createRun(repoRoot, runner, "zoom_video", std::move(payload), clientJobId);
    }
    if (path == "/api/hs/mesh") {
        if (method != "POST") reject(405, "method_not_allowed", "method not allowed");
        Json payload = strictObject(body);
        const std::string clientJobId = requireClientJobId(payload);
        payload = validateHsMesh(std::move(payload));
        return createRun(repoRoot, runner, "hs_mesh", std::move(payload), clientJobId);
    }
    if (path == "/api/transition/mesh") {
        if (method != "POST") reject(405, "method_not_allowed", "method not allowed");
        Json payload = strictObject(body);
        const std::string clientJobId = requireClientJobId(payload);
        payload = validateTransitionMesh(std::move(payload));
        return createRun(repoRoot, runner, "transition_mesh", std::move(payload), clientJobId);
    }
    if (path == "/api/runs/status") {
        if (method != "GET") reject(405, "method_not_allowed", "method not allowed");
        const std::string runId = urlDecode(getQueryParam(query, "runId"));
        return runStatus(repoRoot, runner, runId);
    }
    if (path == "/api/runs/cancel") {
        if (method != "POST") reject(405, "method_not_allowed", "method not allowed");
        const Json payload = strictObject(body);
        if (payload.size() != 1 || !payload.contains("runId"))
            reject(400, "invalid_request", "cancel request only accepts runId");
        const std::string runId = requireString(payload, "runId", 128);
        (void)platformRun(repoRoot, runId);
        (void)computeV1CancelRunRoute(runner, runId, "{}");
        return {200, Json{{"runId", runId}, {"status", "cancel_requested"}}.dump(),
                "application/json", {}};
    }
    reject(404, "not_found", "Compute route was not found");
}

} // namespace

bool isPlatformComputeApiPath(const std::string& path) {
    static const std::set<std::string> paths = {
        "/api/map/render-inline", "/api/map/render", "/api/video/export",
        "/api/hs/mesh", "/api/transition/mesh", "/api/runs/status",
        "/api/runs/cancel",
    };
    return paths.contains(path);
}

bool platformComputeRequestUsesProductionContract(
    const std::filesystem::path& repoRoot, const std::string& path,
    const std::string& query, const std::string& body) {
    if (path == "/api/map/render-inline") {
        try {
            const Json value = Json::parse(body);
            return value.is_object() && value.contains("requestId") &&
                value["requestId"].is_string() && isUuid(value["requestId"].get<std::string>());
        } catch (...) {
            return true;
        }
    }
    try {
        if (path == "/api/map/render" || path == "/api/video/export" ||
            path == "/api/hs/mesh" || path == "/api/transition/mesh") {
            const Json value = Json::parse(body);
            return value.is_object() && value.contains("clientJobId");
        }
        std::string runId;
        if (path == "/api/runs/status") runId = urlDecode(getQueryParam(query, "runId"));
        else if (path == "/api/runs/cancel") {
            const Json value = Json::parse(body);
            if (value.is_object()) runId = value.value("runId", std::string());
        }
        if (runId.empty()) return true;
        const Json params = Json::parse(openDb(repoRoot).getRun(runId).paramsJson);
        return isUuid(params.value("clientJobId", std::string()));
    } catch (...) {
        // Unknown/malformed status and cancellation requests must use the
        // production Problem contract. Existing legacy runs are distinguishable
        // by their persisted params and still take the old route above.
        return path == "/api/runs/status" || path == "/api/runs/cancel";
    }
}

std::string platformComputeRequestId(const std::string& candidate) {
    if (isUuid(candidate)) return candidate;
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::array<unsigned char, 16> bytes{};
    for (std::size_t i = 0; i < bytes.size(); i += 8) {
        const std::uint64_t value = rng();
        for (std::size_t j = 0; j < 8; ++j)
            bytes[i + j] = static_cast<unsigned char>((value >> (j * 8)) & 0xff);
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out << '-';
        out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    }
    return out.str();
}

std::string platformComputeProblemBody(const std::string& code,
                                       const std::string& message,
                                       const std::string& requestId) {
    return Json{{"code", code}, {"message", message}, {"requestId", requestId}}.dump();
}

PlatformComputeResponse platformComputeApiRoute(
    const std::filesystem::path& repoRoot, JobRunner& runner,
    const std::string& method, const std::string& path, const std::string& query,
    const std::string& body, const std::string& requestId) {
    try {
        return routeImpl(repoRoot, runner, method, path, query, body);
    } catch (const ContractError& error) {
        throw HttpError(error.status(), platformComputeProblemBody(
            error.code(), error.what(), requestId));
    } catch (const HttpError& error) {
        int status = error.status();
        if (status == 422) status = 400;
        const std::string code = mappedInternalErrorCode(error.status(), error.body());
        throw HttpError(status, platformComputeProblemBody(
            code, safeInternalMessage(error.status(), code), requestId));
    } catch (const std::exception&) {
        throw HttpError(500, platformComputeProblemBody(
            "compute_failure", "Compute request failed", requestId));
    }
}

} // namespace fsd
