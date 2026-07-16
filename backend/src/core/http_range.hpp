#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

namespace fsd::detail {

struct FileRange {
    bool requested = false;
    bool valid = true;
    std::uintmax_t start = 0;
    std::uintmax_t end = 0;
};

inline bool parseUnsignedDecimal(const std::string& text, std::uintmax_t& value) {
    if (text.empty()) return false;
    std::uintmax_t parsed = 0;
    for (const unsigned char c : text) {
        if (c < '0' || c > '9') return false;
        const std::uintmax_t digit = static_cast<std::uintmax_t>(c - '0');
        if (parsed > (std::numeric_limits<std::uintmax_t>::max() - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    value = parsed;
    return true;
}

inline FileRange parseFileRange(
    const std::string& header,
    std::uintmax_t fileSize,
    bool hasIfRange = false
) {
    FileRange range;
    range.end = fileSize == 0 ? 0 : fileSize - 1;
    if (header.empty() || hasIfRange) return range;

    // Multipart/byteranges is intentionally not emitted. HTTP permits a
    // server to ignore Range, so serve the complete representation instead of
    // incorrectly claiming that a satisfiable multi-range is unsatisfiable.
    if (header.find(',') != std::string::npos) return range;

    range.requested = true;
    if (fileSize == 0 || header.rfind("bytes=", 0) != 0) {
        range.valid = false;
        return range;
    }
    const std::string spec = header.substr(6);
    const std::size_t dash = spec.find('-');
    if (dash == std::string::npos || spec.find('-', dash + 1) != std::string::npos) {
        range.valid = false;
        return range;
    }
    if (dash == 0) {
        std::uintmax_t suffix = 0;
        if (!parseUnsignedDecimal(spec.substr(1), suffix) || suffix == 0) {
            range.valid = false;
            return range;
        }
        range.start = suffix >= fileSize ? 0 : fileSize - suffix;
    } else {
        if (!parseUnsignedDecimal(spec.substr(0, dash), range.start) || range.start >= fileSize) {
            range.valid = false;
            return range;
        }
        if (dash + 1 < spec.size()) {
            std::uintmax_t requestedEnd = 0;
            if (!parseUnsignedDecimal(spec.substr(dash + 1), requestedEnd)) {
                range.valid = false;
                return range;
            }
            range.end = std::min<std::uintmax_t>(requestedEnd, fileSize - 1);
            if (range.end < range.start) {
                range.valid = false;
                return range;
            }
        }
    }
    if (range.end < range.start) range.valid = false;
    return range;
}

} // namespace fsd::detail
