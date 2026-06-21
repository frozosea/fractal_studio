// routes_artifacts.cpp
//
// Scan the runtime/runs/<runId>/ directories and expose every file as an
// artifact. artifactId is "runId:fileName". This keeps artifact listing
// independent of the sqlite table (useful for debugging and tooling).

#include "routes.hpp"
#include "routes_common.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace fsd {

namespace {

std::string lowerExt(const fs::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return ext;
}

std::string kindFromPath(const fs::path& path) {
    const std::string ext = lowerExt(path);
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".gif" || ext == ".webp") return "image";
    if (ext == ".mp4" || ext == ".avi" || ext == ".mov" || ext == ".mkv" || ext == ".webm") return "video";
    if (ext == ".stl") return "stl";
    if (ext == ".glb" || ext == ".gltf") return "mesh";
    if (ext == ".json" || ext == ".csv" || ext == ".txt" || ext == ".md") return "report";
    return "other";
}

std::string contentTypeFromPath(const fs::path& path) {
    const std::string ext = lowerExt(path);
    if (ext == ".png")  return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif")  return "image/gif";
    if (ext == ".webp") return "image/webp";
    if (ext == ".bmp")  return "image/bmp";
    if (ext == ".mp4")  return "video/mp4";
    if (ext == ".webm") return "video/webm";
    if (ext == ".mov")  return "video/quicktime";
    if (ext == ".stl")  return "application/sla";
    if (ext == ".glb")  return "model/gltf-binary";
    if (ext == ".gltf") return "model/gltf+json";
    if (ext == ".json") return "application/json";
    if (ext == ".csv")  return "text/csv";
    return "application/octet-stream";
}

std::string readFileBinary(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("failed to open file: " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct ArtifactEntry {
    std::string artifactId;
    std::string runId;
    std::string fileName;
    std::string path;
    std::string kind;
    long long   sizeBytes = 0;
};

// Append every file in one run directory as an artifact of `runId`.
void collectRunDir(const fs::path& runDir, const std::string& runId, std::vector<ArtifactEntry>& rows) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(runDir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        ArtifactEntry row;
        row.runId      = runId;
        row.path       = it->path().string();
        row.fileName   = it->path().filename().string();
        row.artifactId = runId + ":" + row.fileName;
        row.kind       = kindFromPath(it->path());
        std::error_code sec;
        row.sizeBytes  = static_cast<long long>(fs::file_size(it->path(), sec));
        rows.push_back(row);
    }
}

// Whether a directory directly contains regular files (→ it is a run dir, not a category dir).
bool hasDirectFiles(const fs::path& dir) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) return true;
    }
    return false;
}

std::vector<ArtifactEntry> collectArtifacts(const fs::path& repoRoot) {
    const fs::path runsRoot = repoRoot / "fractal_studio" / "runtime" / "runs";
    std::vector<ArtifactEntry> rows;
    if (!fs::exists(runsRoot)) return rows;

    // Runs are grouped by product type: runs/<category>/<runId>/. A top-level dir that holds
    // loose files is itself an (old, flat) run dir; one that holds only subdirs is a category
    // container whose children are run dirs.
    std::error_code ec;
    for (fs::directory_iterator it(runsRoot, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_directory(ec)) continue;
        const fs::path top = it->path();
        if (hasDirectFiles(top)) {
            collectRunDir(top, top.filename().string(), rows);          // old flat layout
        } else {
            std::error_code cec;
            for (fs::directory_iterator rit(top, cec), rend; rit != rend; rit.increment(cec)) {
                if (cec) break;
                if (rit->is_directory(cec)) collectRunDir(rit->path(), rit->path().filename().string(), rows);
            }
        }
    }
    return rows;
}

fs::path artifactPathFromId(const fs::path& repoRoot, const std::string& artifactId, std::string& fileNameOut) {
    const auto split = artifactId.find(':');
    if (artifactId.empty() || split == std::string::npos) {
        throw std::runtime_error("invalid artifactId");
    }
    const std::string runId    = artifactId.substr(0, split);
    const std::string fileName = artifactId.substr(split + 1);
    if (runId.find("..") != std::string::npos || fileName.find("..") != std::string::npos ||
        runId.find('/')  != std::string::npos || runId.find('\\') != std::string::npos ||
        fileName.find('/') != std::string::npos || fileName.find('\\') != std::string::npos) {
        throw std::runtime_error("invalid artifactId path");
    }
    fileNameOut = fileName;
    const fs::path path = resolveRunDir(repoRoot, runId) / fileName;
    if (!fs::exists(path) || !fs::is_regular_file(path)) {
        throw std::runtime_error("artifact not found");
    }
    return path;
}

} // namespace

std::string artifactsListRoute(const std::filesystem::path& repoRoot, const std::string& query) {
    const std::string kindFilter  = urlDecode(getQueryParam(query, "kind"));
    const std::string runIdFilter = urlDecode(getQueryParam(query, "runId"));

    const auto rows = collectArtifacts(repoRoot);

    Json items = Json::array();
    for (const auto& row : rows) {
        if (!kindFilter.empty()  && row.kind  != kindFilter)  continue;
        if (!runIdFilter.empty() && row.runId != runIdFilter) continue;
        items.push_back({
            {"artifactId",  row.artifactId},
            {"runId",       row.runId},
            {"name",        row.fileName},
            {"kind",        row.kind},
            {"sizeBytes",   row.sizeBytes},
            {"downloadPath","/api/artifacts/download?artifactId=" + row.artifactId},
            {"contentPath", "/api/artifacts/content?artifactId="  + row.artifactId},
            {"localPath",   row.path},
        });
    }
    Json resp = {{"items", items}};
    return resp.dump();
}

std::string artifactDownloadBody(const std::filesystem::path& repoRoot, const std::string& query, std::string& contentType, std::string& downloadName) {
    const std::string artifactId = urlDecode(getQueryParam(query, "artifactId"));
    std::string fileName;
    const fs::path path = artifactPathFromId(repoRoot, artifactId, fileName);
    contentType = contentTypeFromPath(path);
    downloadName = fileName;
    return readFileBinary(path);
}

std::string artifactContentBody(const std::filesystem::path& repoRoot, const std::string& query, std::string& contentType) {
    const std::string artifactId = urlDecode(getQueryParam(query, "artifactId"));
    std::string fileName;
    const fs::path path = artifactPathFromId(repoRoot, artifactId, fileName);
    contentType = contentTypeFromPath(path);
    return readFileBinary(path);
}

} // namespace fsd
