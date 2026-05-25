// routes_common.hpp
//
// Shared JSON/query helpers and Db opener used across the API route files.
// Headers-only so every route TU gets the same helpers.

#pragma once

#include "db.hpp"

#include "../third_party/nlohmann/json.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

namespace fsd {

constexpr int MAX_MAP_DIM = 8192;

using Json = nlohmann::json;

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
