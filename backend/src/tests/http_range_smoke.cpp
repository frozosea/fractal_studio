#include "core/http_range.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        using fsd::detail::parseFileRange;

        const auto full = parseFileRange("", 10);
        require(full.valid && !full.requested && full.start == 0 && full.end == 9,
                "missing Range did not select the complete file");

        const auto first = parseFileRange("bytes=0-0", 10);
        require(first.valid && first.requested && first.start == 0 && first.end == 0,
                "single-byte Range was parsed incorrectly");

        const auto openEnded = parseFileRange("bytes=7-", 10);
        require(openEnded.valid && openEnded.start == 7 && openEnded.end == 9,
                "open-ended Range was parsed incorrectly");

        const auto suffix = parseFileRange("bytes=-4", 10);
        require(suffix.valid && suffix.start == 6 && suffix.end == 9,
                "suffix Range was parsed incorrectly");

        const auto clamped = parseFileRange("bytes=7-99", 10);
        require(clamped.valid && clamped.start == 7 && clamped.end == 9,
                "Range end was not clamped to the representation");

        const auto malformed = parseFileRange("bytes=0-1junk", 10);
        require(malformed.requested && !malformed.valid,
                "malformed single Range was accepted");

        const auto unsatisfied = parseFileRange("bytes=10-", 10);
        require(unsatisfied.requested && !unsatisfied.valid,
                "out-of-bounds Range was accepted");

        const auto multi = parseFileRange("bytes=0-0,2-3", 10);
        require(multi.valid && !multi.requested && multi.start == 0 && multi.end == 9,
                "unsupported multi-range was not ignored");

        const auto conditional = parseFileRange("bytes=2-4", 10, true);
        require(conditional.valid && !conditional.requested &&
                    conditional.start == 0 && conditional.end == 9,
                "unverifiable If-Range did not fall back to the complete file");

        std::cout << "http_range_smoke: single, ignored multi, and If-Range paths passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "http_range_smoke: " << ex.what() << '\n';
        return 1;
    }
}
