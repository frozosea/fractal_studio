#include "http_server.hpp"

#include "hardware_probe.hpp"
#include "routes.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>

namespace fsd {

namespace {
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
    if (status == 404) statusText = "Not Found";
    if (status == 405) statusText = "Method Not Allowed";
    if (status == 409) statusText = "Conflict";
    if (status == 500) statusText = "Internal Server Error";

    std::ostringstream ss;
    ss << "HTTP/1.1 " << status << " " << statusText << "\r\n"
       << "Content-Type: " << contentType << "\r\n"
       << "Access-Control-Allow-Origin: *\r\n"
       << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
       << "Access-Control-Allow-Headers: Content-Type\r\n"
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
    if (method == "POST" && path == "/api/map/field")  return makeHttpResponse(200, mapFieldRoute(repoRoot_, body));
    if (method == "POST" && path == "/api/map/ln")     return makeHttpResponse(200, lnMapRenderRoute(repoRoot_, runner_, body));
    if (method == "POST" && path == "/api/video/zoom")       return makeHttpResponse(200, zoomVideoRoute(repoRoot_, runner_, body));
    if (method == "POST" && path == "/api/video/preview")    return makeHttpResponse(200, videoPreviewRoute(repoRoot_, runner_, body));
    if (method == "POST" && path == "/api/video/export")     return makeHttpResponse(200, videoExportRoute(repoRoot_, runner_, body));
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
    if (method == "GET"  && path == "/api/runs") return makeHttpResponse(200, runsListRoute(repoRoot_, query));
    if (method == "GET"  && path == "/api/tasks/active") return makeHttpResponse(200, activeTasksRoute(runner_));
    if (method == "POST" && path == "/api/runs/cancel") return makeHttpResponse(200, cancelRunRoute(runner_, body));
    if (method == "POST" && path.rfind("/api/runs/", 0) == 0 && path.size() > 17 && path.substr(path.size() - 7) == "/cancel") {
        const std::string runId = path.substr(10, path.size() - 10 - 7);
        return makeHttpResponse(200, cancelRunRoute(runner_, runId, body));
    }

    // Artifacts
    if (method == "GET"  && path == "/api/artifacts") return makeHttpResponse(200, artifactsListRoute(repoRoot_, query));
    if (method == "GET"  && path == "/api/artifacts/download") {
        try {
            std::string contentType, downloadName;
            const std::string bodyText = artifactDownloadBody(repoRoot_, query, contentType, downloadName);
            const std::string headers = "Content-Disposition: attachment; filename=\"" + downloadName + "\"\r\n";
            return makeHttpResponse(200, bodyText, contentType, headers);
        } catch (const std::exception& ex) {
            return makeHttpResponse(404, jsonErrorBody(ex.what()));
        }
    }
    if (method == "GET"  && path == "/api/artifacts/content") {
        try {
            std::string contentType;
            const std::string bodyText = artifactContentBody(repoRoot_, query, contentType);
            return makeHttpResponse(200, bodyText, contentType);
        } catch (const std::exception& ex) {
            return makeHttpResponse(404, jsonErrorBody(ex.what()));
        }
    }

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
void sendAll(int fd, const std::string& data) {
    const char* p = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        // MSG_NOSIGNAL: don't deliver SIGPIPE if the peer has closed the socket.
        // (main.cpp also calls signal(SIGPIPE, SIG_IGN) for defence-in-depth.)
        const ssize_t n = ::send(fd, p, remaining, MSG_NOSIGNAL);
        if (n <= 0) return;  // client gone — stop, don't crash
        p += n;
        remaining -= static_cast<size_t>(n);
    }
}

} // namespace

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
                    const std::string resp = handleRequest(req);
                    sendAll(clientFd, resp);
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
