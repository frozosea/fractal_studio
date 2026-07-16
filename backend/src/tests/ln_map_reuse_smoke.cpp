#include "api/ln_map_reuse.hpp"

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
        nlohmann::json saved = {
            {"centerRe", -0.75},
            {"centerIm", 0.0},
            {"centerReStr", "-0.750000000000000000000000000000000001"},
            {"centerImStr", "0"},
            {"julia", false},
            {"juliaRe", 0.0},
            {"juliaIm", 0.0},
            {"variant", "mandelbrot"},
            {"iterations", 4096},
            {"bailoutSq", 4.0},
            {"colorMap", "spectral1530"},
            {"lnMapColorMode", "hist_eq"},
            {"lnMapCyclesPerOctave", 0.5},
            {"depthOctaves", 20.0},
            {"precisionMode", "standard"},
        };
        fsd::detail::LnMapGenerationIdentity generation;
        fsd::detail::writeLnMapGenerationIdentity(saved, generation);

        auto mismatch = [&](const std::string& re, double bailoutSq = 4.0) {
            return fsd::detail::lnMapIdentityMismatch(
                saved, re, "0", -0.75, 0.0, false, 0.0, 0.0,
                "mandelbrot", 4096, bailoutSq, "spectral1530",
                "hist_eq", 0.5, 20.0, "standard");
        };

        require(mismatch("-0.750000000000000000000000000000000001").empty(),
                "matching ln-map identity was rejected");
        require(mismatch("-0.750000000000000000000000000000000002") == "centerRe",
                "deep centers that collapse to the same double were treated as identical");
        require(mismatch("") == "centerRe",
                "saved precise center was accepted by a numeric-only request");

        nlohmann::json numericOnlySaved = saved;
        numericOnlySaved.erase("centerReStr");
        require(fsd::detail::lnMapIdentityMismatch(
                    numericOnlySaved, saved["centerReStr"].get<std::string>(), "0", -0.75, 0.0,
                    false, 0.0, 0.0, "mandelbrot", 4096, 4.0,
                    "spectral1530", "hist_eq", 0.5, 20.0, "standard") == "centerRe",
                "numeric-only saved center was accepted by a precise request");
        require(mismatch("-0.750000000000000000000000000000000001", 16.0) == "bailoutSq",
                "bailout mismatch was not detected");

        require(fsd::detail::lnMapGenerationIdentityMismatch(saved, generation).empty(),
                "matching generation identity was rejected");
        auto wrongScalar = generation;
        wrongScalar.scalarRequest = "fp64";
        require(fsd::detail::lnMapGenerationIdentityMismatch(saved, wrongScalar) == "lnMapScalar",
                "requested scalar mismatch was not detected");
        auto wrongBailout = generation;
        wrongBailout.bailout = 4.0;
        require(fsd::detail::lnMapGenerationIdentityMismatch(saved, wrongBailout) == "bailout",
                "bailout radius mismatch was not detected");
        auto wrongFastDepth = generation;
        wrongFastDepth.fastFp32DepthOctaves = 12.0;
        require(fsd::detail::lnMapGenerationIdentityMismatch(saved, wrongFastDepth) ==
                    "lnMapFastFp32DepthOctaves",
                "fast precision ladder mismatch was not detected");

        nlohmann::json wrongDepth = saved;
        wrongDepth["depthOctaves"] = 19.5;
        require(fsd::detail::lnMapIdentityMismatch(
                    wrongDepth, saved["centerReStr"].get<std::string>(), "0", -0.75, 0.0,
                    false, 0.0, 0.0, "mandelbrot", 4096, 4.0,
                    "spectral1530", "hist_eq", 0.5, 20.0, "standard") == "depthOctaves",
                "depth mismatch was not detected");

        nlohmann::json missing = saved;
        missing.erase("precisionMode");
        require(fsd::detail::lnMapIdentityMismatch(
                    missing, saved["centerReStr"].get<std::string>(), "0", -0.75, 0.0,
                    false, 0.0, 0.0, "mandelbrot", 4096, 4.0,
                    "spectral1530", "hist_eq", 0.5, 20.0, "standard") == "precisionMode",
                "sidecar missing identity metadata was accepted");

        std::cout << "ln_map_reuse_smoke: compatibility identity checks passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "ln_map_reuse_smoke: " << ex.what() << '\n';
        return 1;
    }
}
