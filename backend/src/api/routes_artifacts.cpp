// routes_artifacts.cpp
//
// Scan categorized runtime/runs/<category>/<runId>/ directories (plus the old
// flat runtime/runs/<runId>/ layout) and expose every file as an artifact.
// artifactId is "runId:relative/path". This keeps artifact listing independent of
// the sqlite table (useful for debugging and tooling).

#include "routes.hpp"
#include "routes_common.hpp"

#include <cstdlib>
#include <filesystem>
#include <iomanip>
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

std::string urlEncodeQueryValue(const std::string& value) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (const unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out << static_cast<char>(c);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return out.str();
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
        std::error_code sec;
        if (it->is_symlink(sec) || sec || !it->is_regular_file(sec)) continue;
        ArtifactEntry row;
        row.runId      = runId;
        row.path       = it->path().string();
        std::error_code rec;
        const fs::path relative = fs::relative(it->path(), runDir, rec);
        if (rec || relative.empty()) continue;
        row.fileName   = relative.generic_string();
        row.artifactId = runId + ":" + row.fileName;
        row.kind       = kindFromPath(it->path());
        std::error_code fec;
        row.sizeBytes  = static_cast<long long>(fs::file_size(it->path(), fec));
        if (fec) continue;
        rows.push_back(row);
    }
}

// Whether a directory directly contains regular files (→ it is a run dir, not a category dir).
bool hasDirectFiles(const fs::path& dir) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code sec;
        if (it->is_symlink(sec) || sec) continue;
        if (it->is_regular_file(sec) && !sec) return true;
    }
    return false;
}

std::vector<ArtifactEntry> collectArtifacts(const fs::path& repoRoot) {
    std::vector<ArtifactEntry> rows;
    std::error_code ec;
    const fs::path runsRoot = fs::canonical(
        repoRoot / "fractal_studio" / "runtime" / "runs", ec);
    if (ec || !fs::is_directory(runsRoot, ec)) return rows;

    // Runs are grouped by product type: runs/<category>/<runId>/. A top-level dir that holds
    // loose files is itself an (old, flat) run dir; one that holds only subdirs is a category
    // container whose children are run dirs.
    for (fs::directory_iterator it(runsRoot, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code sec;
        if (it->is_symlink(sec) || sec || !it->is_directory(sec)) continue;
        const fs::path top = fs::canonical(it->path(), sec);
        if (sec || !canonicalPathIsWithin(runsRoot, top)) continue;
        if (hasDirectFiles(top)) {
            collectRunDir(top, top.filename().string(), rows);          // old flat layout
        } else {
            std::error_code cec;
            for (fs::directory_iterator rit(top, cec), rend; rit != rend; rit.increment(cec)) {
                if (cec) break;
                std::error_code rec;
                if (rit->is_symlink(rec) || rec || !rit->is_directory(rec)) continue;
                const fs::path runDir = fs::canonical(rit->path(), rec);
                if (rec || !canonicalPathIsWithin(runsRoot, runDir)) continue;
                collectRunDir(runDir, rit->path().filename().string(), rows);
            }
        }
    }
    return rows;
}

fs::path artifactPathFromId(
    const fs::path& repoRoot,
    const std::string& artifactId,
    std::string& fileNameOut,
    fs::path& runDirOut
) {
    const auto split = artifactId.find(':');
    if (artifactId.empty() || split == std::string::npos) {
        throw std::runtime_error("invalid artifactId");
    }
    const std::string runId = artifactId.substr(0, split);
    const std::string relativeName = artifactId.substr(split + 1);
    const fs::path path = resolveRunFileSecure(repoRoot, runId, relativeName, &runDirOut);
    fileNameOut = path.filename().string();
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
        const std::string encodedId = urlEncodeQueryValue(row.artifactId);
        items.push_back({
            {"artifactId",  row.artifactId},
            {"runId",       row.runId},
            {"name",        row.fileName},
            {"kind",        row.kind},
            {"sizeBytes",   row.sizeBytes},
            {"downloadPath","/api/artifacts/download?artifactId=" + encodedId},
            {"contentPath", "/api/artifacts/content?artifactId="  + encodedId},
            {"localPath",   row.path},
        });
    }
    Json resp = {{"items", items}};
    return resp.dump();
}

ArtifactFile artifactFileRoute(const std::filesystem::path& repoRoot, const std::string& query) {
    const std::string artifactId = urlDecode(getQueryParam(query, "artifactId"));
    std::string fileName;
    fs::path runDir;
    const fs::path path = artifactPathFromId(repoRoot, artifactId, fileName, runDir);
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) throw std::runtime_error("failed to stat artifact");
    return ArtifactFile{path, runDir, contentTypeFromPath(path), fileName, size};
}

} // namespace fsd
