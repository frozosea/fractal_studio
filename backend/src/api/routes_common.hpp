// routes_common.hpp
//
// Shared JSON/query helpers and Db opener used across the API route files.
// Headers-only so every route TU gets the same helpers.

#pragma once

#include "db.hpp"

#include "../third_party/nlohmann/json.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>

namespace fsd {

constexpr int MAX_MAP_DIM = 8192;

using Json = nlohmann::json;

// A request that supplies a high-precision center-coordinate string (deep zooms need it
// for perturbation reference orbits) must have that string win over the plain double —
// otherwise a request that only sends the string, or sends a stale/mismatched double
// alongside it, has the standard (non-perturbation) renderer silently fall back to the
// double field's default instead of the intended point.
inline double resolveCenterCoord(const std::string& preciseStr, double fallback) {
    if (!preciseStr.empty()) {
        try {
            return std::stod(preciseStr);
        } catch (const std::exception&) {
            // Malformed string: fall back to the plain double rather than throw here —
            // callers that need strict validation already parse the string themselves
            // downstream (e.g. the perturbation reference-orbit path).
        }
    }
    return fallback;
}

inline bool pathHasControlCharacters(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    });
}

inline bool canonicalPathIsWithin(
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

inline void validateRunIdForPath(const std::string& runId) {
    const std::filesystem::path parsed(runId);
    if (runId.empty() || runId.find("..") != std::string::npos ||
        runId.find('/') != std::string::npos || runId.find('\\') != std::string::npos ||
        pathHasControlCharacters(runId) || parsed.is_absolute() || parsed.has_root_path()) {
        throw std::runtime_error("invalid runId path");
    }
}

// Resolve an existing run without following category/run directory symlinks.
// The returned canonical directory is guaranteed to remain beneath the
// canonical runtime/runs root at resolution time.
inline std::filesystem::path resolveRunDirSecure(
    const std::filesystem::path& repoRoot,
    const std::string& runId
) {
    namespace fs = std::filesystem;
    validateRunIdForPath(runId);

    std::error_code ec;
    const fs::path runsRoot = fs::canonical(
        repoRoot / "fractal_studio" / "runtime" / "runs", ec);
    if (ec || !fs::is_directory(runsRoot, ec)) {
        throw std::runtime_error("run not found");
    }

    auto resolveCandidate = [&](const fs::path& candidate) -> fs::path {
        std::error_code sec;
        const fs::file_status status = fs::symlink_status(candidate, sec);
        if (!sec && fs::is_symlink(status)) {
            throw std::runtime_error("invalid run directory symlink");
        }
        if (sec || !fs::is_directory(status)) return {};

        const fs::path canonical = fs::canonical(candidate, sec);
        if (sec || !fs::is_directory(canonical, sec) ||
            !canonicalPathIsWithin(runsRoot, canonical)) {
            throw std::runtime_error("invalid run directory path");
        }
        return canonical;
    };

    if (const fs::path flat = resolveCandidate(runsRoot / runId); !flat.empty()) {
        return flat;
    }

    for (fs::directory_iterator it(runsRoot, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code dec;
        if (it->is_symlink(dec) || dec || !it->is_directory(dec)) continue;
        if (const fs::path nested = resolveCandidate(it->path() / runId); !nested.empty()) {
            return nested;
        }
    }
    throw std::runtime_error("run not found");
}

inline std::filesystem::path resolveRunFileSecure(
    const std::filesystem::path& repoRoot,
    const std::string& runId,
    const std::string& relativeName,
    std::filesystem::path* resolvedRunDir = nullptr
) {
    namespace fs = std::filesystem;
    if (relativeName.empty() || relativeName.find('\\') != std::string::npos ||
        pathHasControlCharacters(relativeName)) {
        throw std::runtime_error("invalid run-relative path");
    }
    const fs::path relativePath(relativeName);
    if (relativePath.is_absolute() || relativePath.has_root_path()) {
        throw std::runtime_error("invalid run-relative path");
    }
    for (const auto& component : relativePath) {
        if (component == "." || component == ".." || component.empty()) {
            throw std::runtime_error("invalid run-relative path");
        }
    }

    const fs::path runDir = resolveRunDirSecure(repoRoot, runId);
    std::error_code ec;
    const fs::path path = fs::weakly_canonical(runDir / relativePath, ec);
    if (ec || !fs::is_regular_file(path, ec)) {
        throw std::runtime_error("run file not found");
    }
    if (!canonicalPathIsWithin(runDir, path)) {
        throw std::runtime_error("invalid run-relative path");
    }
    if (resolvedRunDir != nullptr) *resolvedRunDir = runDir;
    return path;
}

inline Json parseJsonBody(const std::string& body) {
    if (body.empty()) return Json::object();
    try {
        return Json::parse(body);
    } catch (const std::exception&) {
        return Json::object();
    }
}

inline std::string getQueryParam(const std::string& query, const std::string& key) {
    const std::regex re("(?:^|&)" + key + "=([^&]*)");
    std::smatch m;
    if (std::regex_search(query, m, re)) {
        return m[1].str();
    }
    return "";
}

inline std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const std::string hex = s.substr(i + 1, 2);
            const char c = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
            out.push_back(c);
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// Base64 encoder — used by any route that returns binary data in JSON.
inline std::string base64Encode(const uint8_t* data, size_t len) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = (i + 1 < len) ? data[i + 1] : 0;
        const uint32_t b2 = (i + 2 < len) ? data[i + 2] : 0;
        const uint32_t v  = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? tbl[(v >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? tbl[(v >> 0) & 0x3F] : '=');
    }
    return out;
}

inline Db openDb(const std::filesystem::path& repoRoot) {
    namespace fs = std::filesystem;
    const fs::path dbDir = repoRoot / "fractal_studio" / "runtime" / "db";
    fs::create_directories(dbDir);
    Db db(dbDir / "fractal_studio.sqlite3");
    db.ensureSchema();
    return db;
}

inline std::filesystem::path repoRuntime(const std::filesystem::path& repoRoot) {
    return repoRoot / "fractal_studio" / "runtime";
}

inline void atomicWriteText(const std::filesystem::path& path, const std::string& text) {
    namespace fs = std::filesystem;
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream os(tmp, std::ios::binary);
        os << text;
        if (!os) {
            std::error_code ec;
            fs::remove(tmp, ec);
            throw std::runtime_error("atomic write failed: " + path.string());
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
    }
    if (ec) throw std::runtime_error("atomic write failed: " + path.string());
}

} // namespace fsd
