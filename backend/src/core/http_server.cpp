#include "http_server.hpp"

#include "hardware_probe.hpp"
#include "http_range.hpp"
#include "routes.hpp"
#include "../api/routes_common.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <array>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>

namespace fsd {

namespace {
std::string requestHeaderValue(const std::string& request, const std::string& wantedName);

bool envEnabled(const char* name, bool defaultValue) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') return defaultValue;
    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value != "0" && value != "false" && value != "off" && value != "no";
}

bool constantTimeEqual(const std::string& left, const std::string& right) {
    const std::size_t count = std::max(left.size(), right.size());
    unsigned char diff = static_cast<unsigned char>(left.size() ^ right.size());
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned char a = i < left.size() ? static_cast<unsigned char>(left[i]) : 0;
        const unsigned char b = i < right.size() ? static_cast<unsigned char>(right[i]) : 0;
        diff = static_cast<unsigned char>(diff | (a ^ b));
    }
    return diff == 0;
}

bool computeAuthorized(const std::string& request) {
    const char* configured = std::getenv("FSD_COMPUTE_SERVICE_KEY");
    if (configured == nullptr || *configured == '\0') return false;
    const std::string authorization = requestHeaderValue(request, "Authorization");
    constexpr const char* prefix = "Bearer ";
    if (authorization.rfind(prefix, 0) != 0) return false;
    return constantTimeEqual(authorization.substr(std::strlen(prefix)), configured);
}

std::string jsonEscapeString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    out += ' ';
                else
                    out += c;
        }
    }
    return out;
}

std::string jsonErrorBody(const std::string& msg) {
    return "{\"error\":\"" + jsonEscapeString(msg) + "\"}";
}
} // namespace

HttpServer::HttpServer(int port, JobRunner& runner, std::filesystem::path repoRoot)
    : port_(port), runner_(runner), repoRoot_(std::move(repoRoot)) {}

std::string HttpServer::makeHttpResponse(int status, const std::string& body, const std::string& contentType, const std::string& extraHeaders) {
    const char* statusText = "OK";
    if (status == 201) statusText = "Created";
    if (status == 204) statusText = "No Content";
    if (status == 400) statusText = "Bad Request";
    if (status == 401) statusText = "Unauthorized";
    if (status == 403) statusText = "Forbidden";
    if (status == 404) statusText = "Not Found";
    if (status == 405) statusText = "Method Not Allowed";
    if (status == 409) statusText = "Conflict";
    if (status == 410) statusText = "Gone";
    if (status == 413) statusText = "Payload Too Large";
    if (status == 422) statusText = "Unprocessable Entity";
    if (status == 429) statusText = "Too Many Requests";
    if (status == 500) statusText = "Internal Server Error";
    if (status == 503) statusText = "Service Unavailable";

    std::ostringstream ss;
    ss << "HTTP/1.1 " << status << " " << statusText << "\r\n"
       << "Content-Type: " << contentType << "\r\n"
       << "Access-Control-Allow-Origin: *\r\n"
       << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
       << "Access-Control-Allow-Headers: Content-Type, Authorization, Range, If-Range, X-Request-Id\r\n"
       << "Access-Control-Expose-Headers: "
       << "X-FSD-Status, X-FSD-Request-Id, X-FSD-Generated-Ms, X-FSD-Engine, "
       << "X-FSD-Scalar, X-FSD-Width, X-FSD-Height, X-FSD-Pixel-Format\r\n"
       << extraHeaders
       << "Content-Length: " << body.size() << "\r\n"
       << "Connection: close\r\n\r\n"
       << body;
    return ss.str();
}

std::string HttpServer::handleRequest(const std::string& request) const {
    try {
    auto splitPathAndQuery = [](const std::string& rawPath) {
        const std::size_t q = rawPath.find('?');
        if (q == std::string::npos) return std::make_tuple(rawPath, std::string());
        return std::make_tuple(rawPath.substr(0, q), rawPath.substr(q + 1));
    };

    const std::size_t headerEnd = request.find("\r\n\r\n");
    const std::string head = (headerEnd == std::string::npos) ? request : request.substr(0, headerEnd);
    const std::string body = (headerEnd == std::string::npos) ? std::string() : request.substr(headerEnd + 4);

    std::istringstream in(head);
    std::string method, rawPath, version;
    in >> method >> rawPath >> version;

    const auto [path, query] = splitPathAndQuery(rawPath);

    if (method == "OPTIONS") return makeHttpResponse(200, "{}");

    // Private Compute v1. Liveness is deliberately unauthenticated so a
    // private-network load balancer can probe the process. Every other route
    // requires the service credential even when legacy routes remain enabled.
    if (method == "GET" && path == "/compute/v1/health") {
        return makeHttpResponse(200, computeV1HealthRoute());
    }
    if (path.rfind("/compute/v1/", 0) == 0 && !computeAuthorized(request)) {
        return makeHttpResponse(401,
            "{\"error\":{\"code\":\"COMPUTE_UNAUTHORIZED\",\"message\":\"valid Compute service credential required\",\"details\":{}}}");
    }
    if (method == "GET" && path == "/compute/v1/capabilities") {
        return makeHttpResponse(200, computeV1CapabilitiesRoute());
    }
    if (method == "POST" && path == "/compute/v1/previews") {
        const Json envelope = Json::parse(body);
        if (envelope.value("schemaVersion", 0) == 1 &&
            envelope.value("kind", std::string()) == "map_image" &&
            envelope.contains("payload") && envelope["payload"].is_object()) {
            computeV1ValidateOrbitRequest(body, false);
            int status = 200;
            std::string contentType;
            std::string extraHeaders;
            const std::string frame = mapRenderInlineRoute(
                repoRoot_, envelope["payload"].dump(), status, contentType, extraHeaders);
            return makeHttpResponse(status, frame, contentType, extraHeaders);
        }
        return makeHttpResponse(200, computeV1PreviewJsonRoute(repoRoot_, runner_, body));
    }
    if (method == "POST" && path == "/compute/v1/runs") {
        return makeHttpResponse(202, computeV1CreateRunRoute(repoRoot_, runner_, body));
    }
    if (path.rfind("/compute/v1/runs/", 0) == 0) {
        const std::string tail = path.substr(std::strlen("/compute/v1/runs/"));
        const std::string manifestSuffix = "/manifest";
        const std::string cancelSuffix = "/cancel";
        if (method == "GET" && tail.size() > manifestSuffix.size() &&
            tail.substr(tail.size() - manifestSuffix.size()) == manifestSuffix) {
            const std::string runId = tail.substr(0, tail.size() - manifestSuffix.size());
            return makeHttpResponse(200, computeV1ManifestRoute(repoRoot_, runner_, runId));
        }
        if (method == "POST" && tail.size() > cancelSuffix.size() &&
            tail.substr(tail.size() - cancelSuffix.size()) == cancelSuffix) {
            const std::string runId = tail.substr(0, tail.size() - cancelSuffix.size());
            return makeHttpResponse(202, computeV1CancelRunRoute(runner_, runId, body));
        }
        if (method == "GET" && tail.find('/') == std::string::npos && !tail.empty()) {
            return makeHttpResponse(200, computeV1RunStatusRoute(repoRoot_, runner_, tail));
        }
    }

    // Production contract consumed by platform-backend. During the local
    // migration, a valid service key plus the production DTO shape selects
    // this adapter; authenticated legacy-shaped requests continue below. In a
    // hosted build (legacy disabled), these paths always select this contract.
    const bool legacyApiEnabled = envEnabled("FSD_ENABLE_LEGACY_API", true);
    if (isPlatformComputeApiPath(path)) {
        const std::string authorization = requestHeaderValue(request, "Authorization");
        const bool hasAuthorization = !authorization.empty();
        const bool productionRequest = !legacyApiEnabled ||
            platformComputeRequestUsesProductionContract(repoRoot_, path, query, body);
        if (productionRequest || hasAuthorization) {
            std::string requestIdCandidate = requestHeaderValue(request, "X-Request-Id");
            if (requestIdCandidate.empty() && !body.empty()) {
                const Json parsedBody = parseJsonBody(body);
                if (parsedBody.is_object() && parsedBody.contains("requestId") &&
                    parsedBody["requestId"].is_string()) {
                    requestIdCandidate = parsedBody["requestId"].get<std::string>();
                }
            }
            const std::string requestId = platformComputeRequestId(requestIdCandidate);
            if (!computeAuthorized(request)) {
                return makeHttpResponse(401, platformComputeProblemBody(
                    "compute_unauthorized",
                    "valid Compute service credential required", requestId));
            }
            if (productionRequest) {
                const PlatformComputeResponse response = platformComputeApiRoute(
                    repoRoot_, runner_, method, path, query, body, requestId);
                return makeHttpResponse(response.status, response.body,
                    response.contentType, response.extraHeaders);
            }
            // A valid key on a legacy-shaped request authenticates the local
            // frontend, but does not silently reinterpret its response DTO.
        }
    }

    if (path.rfind("/api/", 0) == 0 && !legacyApiEnabled) {
        return makeHttpResponse(404, "{\"error\":\"legacy API disabled\"}");
    }

    // System
    if (method == "GET"  && path == "/api/system/check")    return makeHttpResponse(200, systemCheckRoute());
    if (method == "GET"  && path == "/api/system/hardware") return makeHttpResponse(200, systemHardwareRoute());
    if (method == "GET"  && path == "/api/system/capabilities") return makeHttpResponse(200, systemCapabilitiesRoute());

    // Map (native)
    if (method == "POST" && path == "/api/map/render") return makeHttpResponse(200, mapRenderRoute(repoRoot_, runner_, body));
    if (method == "POST" && path == "/api/map/render-inline") {
        int status = 200;
        std::string contentType;
        std::string extraHeaders;
        const std::string bodyText = mapRenderInlineRoute(repoRoot_, body, status, contentType, extraHeaders);
        return makeHttpResponse(status, bodyText, contentType, extraHeaders);
    }
    if (method == "POST" && path == "/api/map/preempt") return makeHttpResponse(200, mapPreemptRoute(body));
    if (method == "POST" && path == "/api/map/field/session/start")
        return makeHttpResponse(200, mapFieldSessionStartRoute(repoRoot_, body));
    if (method == "POST" && path == "/api/map/field/session/status")
        return makeHttpResponse(200, mapFieldSessionStatusRoute(body));
    if (method == "POST" && path == "/api/map/field/session/snapshot")
        return makeHttpResponse(200, mapFieldSessionSnapshotRoute(body));
    if (method == "POST" && path == "/api/map/field/session/result")
        return makeHttpResponse(200, mapFieldSessionResultRoute(body));
    if (method == "POST" && path == "/api/map/field/session/ack")
        return makeHttpResponse(200, mapFieldSessionAcknowledgeRoute(body));
    if (method == "POST" && path == "/api/map/field")  return makeHttpResponse(200, mapFieldRoute(repoRoot_, body));
    if (method == "POST" && path == "/api/map/ln")     return makeHttpResponse(200, lnMapRenderRoute(repoRoot_, runner_, body));
    if (method == "POST" && path == "/api/video/zoom")       return makeHttpResponse(200, zoomVideoRoute(repoRoot_, runner_, body));
    if (method == "POST" && path == "/api/video/preview")    return makeHttpResponse(200, videoPreviewRoute(repoRoot_, runner_, body));
    if (method == "POST" && path == "/api/video/export")     return makeHttpResponse(200, videoExportRoute(repoRoot_, runner_, body));
    if (method == "POST" && path == "/api/video/transition")         return makeHttpResponse(200, transitionVideoExportRoute(repoRoot_, runner_, body));
    if (method == "POST" && path == "/api/video/transition-preview") return makeHttpResponse(200, transitionVideoPreviewRoute(repoRoot_, runner_, body));
    if (method == "POST" && path == "/api/hs/mesh")              return makeHttpResponse(200, hsMeshRoute(repoRoot_, runner_, body));
    if (method == "POST" && path == "/api/hs/field")             return makeHttpResponse(200, hsFieldRoute(repoRoot_, runner_, body));
    if (method == "POST" && path == "/api/transition/mesh")      return makeHttpResponse(200, transitionMeshRoute(repoRoot_, runner_, body));
    if (method == "POST" && path == "/api/transition/voxels")    return makeHttpResponse(200, transitionVoxelsRoute(repoRoot_, runner_, body));

    // Special points
    if (method == "POST" && path == "/api/special-points/auto") return makeHttpResponse(200, specialPointsAutoRoute(repoRoot_, body));
    if (method == "POST" && path == "/api/special-points/seed") return makeHttpResponse(200, specialPointsSeedRoute(repoRoot_, body));
    if (method == "POST" && path == "/api/special-points/enumerate") return makeHttpResponse(200, specialPointsEnumerateRoute(repoRoot_, runner_, body));
    if (method == "POST" && path == "/api/special-points/search") return makeHttpResponse(200, specialPointsSearchRoute(repoRoot_, runner_, body));
    if (method == "GET"  && path == "/api/special-points/results") return makeHttpResponse(200, specialPointsResultsRoute(repoRoot_, runner_, query));
    if (method == "POST" && path == "/api/special-points/snap") return makeHttpResponse(200, specialPointsSnapRoute(body));
    if (method == "GET"  && path == "/api/special-points")      return makeHttpResponse(200, specialPointsListRoute(repoRoot_, query));

    // Benchmark
    if (method == "POST" && path == "/api/benchmark") return makeHttpResponse(200, benchmarkRoute(runner_, body));

    // Custom variants
    if (method == "POST" && path == "/api/variants/compile") return makeHttpResponse(200, variantCompileRoute(repoRoot_, body));
    if (method == "GET"  && path == "/api/variants")         return makeHttpResponse(200, variantListRoute(repoRoot_));
    if (method == "POST" && path == "/api/variants/delete")  return makeHttpResponse(200, variantDeleteRoute(repoRoot_, body));

    // Runs
    if (method == "GET"  && path == "/api/runs/status") return makeHttpResponse(200, runStatusRoute(repoRoot_, runner_, query));
    if (method == "GET"  && path == "/api/runs") return makeHttpResponse(200, runsListRoute(repoRoot_, runner_, query));
    if (method == "GET"  && path == "/api/tasks/active") return makeHttpResponse(200, activeTasksRoute(runner_));
    if (method == "POST" && path == "/api/runs/cancel") return makeHttpResponse(200, cancelRunRoute(runner_, body));
    if (method == "POST" && path.rfind("/api/runs/", 0) == 0 && path.size() > 17 && path.substr(path.size() - 7) == "/cancel") {
        const std::string runId = path.substr(10, path.size() - 10 - 7);
        return makeHttpResponse(200, cancelRunRoute(runner_, runId, body));
    }

    // Artifacts
    if (method == "GET"  && path == "/api/artifacts") return makeHttpResponse(200, artifactsListRoute(repoRoot_, query));
    // Artifact bodies are streamed directly by streamArtifactResponse() in the
    // connection worker, so multi-gigabyte files never enter a std::string.

    if (method != "GET" && method != "POST") return makeHttpResponse(405, "{\"error\":\"method not allowed\"}");
    return makeHttpResponse(404, "{\"error\":\"not found\"}");
    } catch (const HttpError& ex) {
        return makeHttpResponse(ex.status(), ex.body());
    } catch (const std::exception& ex) {
        return makeHttpResponse(500, jsonErrorBody(ex.what()));
    }
}

namespace {

// Read one HTTP request from a client socket. Handles Content-Length so
// large JSON POST bodies (> 16 KiB) are fully received before dispatch.
std::string readRequest(int fd) {
    std::string buf;
    buf.reserve(4096);
    char tmp[8192];

    // Read headers until \r\n\r\n.
    while (true) {
        const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return buf;
        buf.append(tmp, static_cast<size_t>(n));
        if (buf.find("\r\n\r\n") != std::string::npos) break;
        if (buf.size() > (1u << 24)) return buf; // safety cap: 16 MiB of headers
    }

    // Find content length.
    const std::size_t headerEnd = buf.find("\r\n\r\n");
    std::size_t contentLength = 0;
    {
        const std::string headers = buf.substr(0, headerEnd);
        const std::string key = "Content-Length:";
        std::size_t p = 0;
        while (p < headers.size()) {
            std::size_t eol = headers.find("\r\n", p);
            if (eol == std::string::npos) eol = headers.size();
            const std::string line = headers.substr(p, eol - p);
            if (line.size() >= key.size()) {
                // case-insensitive compare for the header name
                bool match = true;
                for (size_t i = 0; i < key.size(); i++) {
                    char a = std::tolower(static_cast<unsigned char>(line[i]));
                    char b = std::tolower(static_cast<unsigned char>(key[i]));
                    if (a != b) { match = false; break; }
                }
                if (match) {
                    std::string v = line.substr(key.size());
                    // trim
                    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
                    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
                    try { contentLength = static_cast<std::size_t>(std::stoul(v)); } catch (...) {}
                    break;
                }
            }
            p = eol + 2;
        }
    }

    // Read remainder of body if needed.
    const std::size_t bodyStart = headerEnd + 4;
    while (buf.size() < bodyStart + contentLength) {
        const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, static_cast<size_t>(n));
        if (buf.size() > (1u << 30)) break; // 1 GiB safety cap
    }
    return buf;
}

// Send all bytes, looping on partial writes (response can be many MB for
// artifact content downloads).
bool sendBytes(int fd, const char* p, std::size_t remaining) {
    while (remaining > 0) {
        // MSG_NOSIGNAL: don't deliver SIGPIPE if the peer has closed the socket.
        // (main.cpp also calls signal(SIGPIPE, SIG_IGN) for defence-in-depth.)
        const ssize_t n = ::send(fd, p, remaining, MSG_NOSIGNAL);
        if (n <= 0) return false;  // client gone — stop, don't crash
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

void sendAll(int fd, const std::string& data) {
    (void)sendBytes(fd, data.data(), data.size());
}

std::string requestHeaderValue(const std::string& request, const std::string& wantedName) {
    const std::size_t headEnd = request.find("\r\n\r\n");
    const std::size_t limit = headEnd == std::string::npos ? request.size() : headEnd;
    std::size_t lineStart = request.find("\r\n");
    if (lineStart == std::string::npos) return {};
    lineStart += 2;
    while (lineStart < limit) {
        std::size_t lineEnd = request.find("\r\n", lineStart);
        if (lineEnd == std::string::npos || lineEnd > limit) lineEnd = limit;
        const std::size_t colon = request.find(':', lineStart);
        if (colon != std::string::npos && colon < lineEnd) {
            const std::size_t nameLength = colon - lineStart;
            bool matches = nameLength == wantedName.size();
            for (std::size_t i = 0; matches && i < nameLength; ++i) {
                matches = std::tolower(static_cast<unsigned char>(request[lineStart + i])) ==
                          std::tolower(static_cast<unsigned char>(wantedName[i]));
            }
            if (matches) {
                std::size_t first = colon + 1;
                while (first < lineEnd && (request[first] == ' ' || request[first] == '\t')) ++first;
                std::size_t last = lineEnd;
                while (last > first && (request[last - 1] == ' ' || request[last - 1] == '\t')) --last;
                return request.substr(first, last - first);
            }
        }
        lineStart = lineEnd + 2;
    }
    return {};
}

std::string safeDownloadName(const std::string& name) {
    std::string safe;
    safe.reserve(name.size());
    for (const unsigned char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == ' ') {
            safe.push_back(static_cast<char>(c));
        } else {
            safe.push_back('_');
        }
    }
    return safe.empty() ? "artifact" : safe;
}

class ScopedFileDescriptor {
public:
    explicit ScopedFileDescriptor(int fd) : fd_(fd) {}
    ~ScopedFileDescriptor() {
        if (fd_ >= 0) ::close(fd_);
    }
    ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
    ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;
    int get() const { return fd_; }

private:
    int fd_ = -1;
};

bool pathIsWithin(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate
) {
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() || *rootIt != *candidateIt) return false;
    }
    return true;
}

} // namespace

bool HttpServer::streamArtifactResponse(int clientFd, const std::string& request) const {
    const std::size_t firstLineEnd = request.find("\r\n");
    const std::string firstLine = request.substr(0, firstLineEnd);
    std::istringstream firstLineStream(firstLine);
    std::string method, rawPath, version;
    firstLineStream >> method >> rawPath >> version;
    const std::size_t q = rawPath.find('?');
    const std::string path = rawPath.substr(0, q);
    const std::string query = q == std::string::npos ? std::string() : rawPath.substr(q + 1);
    const bool isDownload = path == "/api/artifacts/download";
    const bool isLegacyContent = path == "/api/artifacts/content";
    const bool isComputeContent = path == "/compute/v1/artifacts";
    const bool isContent = isLegacyContent || isComputeContent;
    if (method != "GET" || (!isDownload && !isContent)) return false;

    if (isComputeContent && !computeAuthorized(request)) {
        sendAll(clientFd,
            makeHttpResponse(401,
                "{\"error\":{\"code\":\"COMPUTE_UNAUTHORIZED\",\"message\":\"valid Compute service credential required\",\"details\":{}}}"));
        return true;
    }
    if (!isComputeContent && !envEnabled("FSD_ENABLE_LEGACY_API", true)) {
        sendAll(clientFd, makeHttpResponse(404, "{\"error\":\"legacy API disabled\"}"));
        return true;
    }

    try {
        const ArtifactFile file = artifactFileRoute(repoRoot_, query);
        ScopedFileDescriptor input(::open(
            file.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        if (input.get() < 0) throw std::runtime_error("failed to open artifact");

        struct stat fileStat {};
        if (::fstat(input.get(), &fileStat) != 0 || !S_ISREG(fileStat.st_mode) || fileStat.st_size < 0) {
            throw std::runtime_error("failed to stat artifact");
        }
        const std::uintmax_t fileSize = static_cast<std::uintmax_t>(fileStat.st_size);

        // Resolve the already-open descriptor and re-check containment. This
        // closes the canonicalize/open race if a local process swaps a parent
        // directory or symlink between route validation and open().
        std::error_code openedPathError;
        const std::filesystem::path openedPath = std::filesystem::canonical(
            "/proc/self/fd/" + std::to_string(input.get()), openedPathError);
        if (openedPathError || !pathIsWithin(file.runDir, openedPath)) {
            throw std::runtime_error("artifact path changed during open");
        }

        const std::string rangeHeader = requestHeaderValue(request, "Range");
        // This endpoint does not publish an ETag or Last-Modified validator.
        // It therefore cannot prove an If-Range match and must send the full
        // representation rather than risk combining bytes from two versions.
        const detail::FileRange range = detail::parseFileRange(
            rangeHeader, fileSize,
            !requestHeaderValue(request, "If-Range").empty());
        if (!range.valid) {
            const std::string response =
                "HTTP/1.1 416 Range Not Satisfiable\r\n"
                "Content-Range: bytes */" + std::to_string(fileSize) + "\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(clientFd, response);
            return true;
        }

        const std::uintmax_t contentLength = fileSize == 0 ? 0 : range.end - range.start + 1;
        if (contentLength > 0) {
            if (range.start > static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max())) {
                throw std::runtime_error("artifact offset is too large");
            }
            if (::lseek(input.get(), static_cast<off_t>(range.start), SEEK_SET) < 0) {
                throw std::runtime_error("failed to seek artifact");
            }
        }

        std::ostringstream head;
        head << "HTTP/1.1 " << (range.requested ? "206 Partial Content" : "200 OK") << "\r\n"
             << "Content-Type: " << file.contentType << "\r\n"
             << "Access-Control-Allow-Origin: *\r\n"
             << "Access-Control-Expose-Headers: Content-Disposition, Content-Length, Content-Range, Accept-Ranges\r\n"
             << "Accept-Ranges: bytes\r\n";
        if (range.requested) {
            head << "Content-Range: bytes " << range.start << '-' << range.end << '/' << fileSize << "\r\n";
        }
        if (isDownload) {
            head << "Content-Disposition: attachment; filename=\"" << safeDownloadName(file.downloadName) << "\"\r\n";
        }
        head << "Content-Length: " << contentLength << "\r\nConnection: close\r\n\r\n";
        const std::string headerText = head.str();
        if (!sendBytes(clientFd, headerText.data(), headerText.size()) || contentLength == 0) return true;

        std::array<char, 1024 * 1024> chunk{};
        std::uintmax_t remaining = contentLength;
        while (remaining > 0) {
            const std::size_t wanted = static_cast<std::size_t>(
                std::min<std::uintmax_t>(remaining, chunk.size()));
            ssize_t got = -1;
            do {
                got = ::read(input.get(), chunk.data(), wanted);
            } while (got < 0 && errno == EINTR);
            if (got <= 0) break;
            if (!sendBytes(clientFd, chunk.data(), static_cast<std::size_t>(got))) break;
            remaining -= static_cast<std::uintmax_t>(got);
        }
    } catch (const std::exception& ex) {
        sendAll(clientFd, makeHttpResponse(404, jsonErrorBody(ex.what())));
    }
    return true;
}

void HttpServer::serveForever() {
    const int serverFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) throw std::runtime_error("failed to create server socket");

    int opt = 1;
    ::setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if (::bind(serverFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(serverFd);
        throw std::runtime_error("failed to bind server socket");
    }

    if (::listen(serverFd, 64) < 0) {
        ::close(serverFd);
        throw std::runtime_error("failed to listen");
    }

    std::cout << "HTTP server listening on 0.0.0.0:" << port_ << std::endl;

    while (true) {
        const int clientFd = ::accept(serverFd, nullptr, nullptr);
        if (clientFd < 0) continue;

        // Detached thread per connection — allows concurrent renders.
        // All exceptions inside the thread are caught so a bad request
        // (including a CUDA error) never takes down the server process.
        std::thread([this, clientFd]() {
            try {
                const std::string req = readRequest(clientFd);
                if (!req.empty()) {
                    if (!streamArtifactResponse(clientFd, req)) {
                        const std::string resp = handleRequest(req);
                        sendAll(clientFd, resp);
                    }
                }
            } catch (...) {
                // Last-resort catch: log and continue serving.
                const std::string err = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                sendAll(clientFd, err);
            }
            ::close(clientFd);
        }).detach();
    }
}

} // namespace fsd
