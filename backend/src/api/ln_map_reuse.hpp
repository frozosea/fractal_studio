#pragma once

#include "../third_party/nlohmann/json.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace fsd::detail {

using ReuseJson = nlohmann::json;

struct LnMapGenerationIdentity {
    double bailout = 2.0;
    double extraOctaves = 7.0;
    std::string engineRequest = "auto";
    std::string scalarRequest = "auto";
    double fastFp32DepthOctaves = 18.0;
    double fastFp64DepthOctaves = 34.0;
    bool fastValidate = true;
    double fastValidationBandOctaves = 4.0;
    int fastValidationSampleRows = 5;
    int fastValidationSampleCols = 24;
    double fastValidationMaxMismatchRatio = 0.01;
    int fastValidationMaxP99IterDelta = 16;
    double fastValidationMaxMeanColorDelta = 8.0;
};

inline bool lnMapNumberMatches(
    const ReuseJson& saved,
    const char* key,
    double requested,
    double relTolerance = 0.0
) {
    if (!saved.contains(key) || !saved[key].is_number()) return false;
    const double actual = saved[key].get<double>();
    if (!std::isfinite(actual) || !std::isfinite(requested)) return false;
    const double scale = std::max({1.0, std::abs(actual), std::abs(requested)});
    return std::abs(actual - requested) <= relTolerance * scale;
}

inline bool lnMapCoordinateMatches(
    const ReuseJson& saved,
    const char* numberKey,
    const char* stringKey,
    const std::string& requestedString,
    double requestedNumber
) {
    const std::string savedString = saved.value(stringKey, std::string());
    // Precise coordinates are identity data, not display labels. Requiring the
    // exact decimal string prevents distinct deep-zoom centers that collapse to
    // the same double/long-double value from sharing a cached histogram or strip.
    if (requestedString.empty() != savedString.empty()) return false;
    if (!requestedString.empty()) return savedString == requestedString;
    return lnMapNumberMatches(saved, numberKey, requestedNumber);
}

inline std::string lnMapIdentityMismatch(
    const ReuseJson& saved,
    const std::string& centerReStr,
    const std::string& centerImStr,
    double centerRe,
    double centerIm,
    bool julia,
    double juliaRe,
    double juliaIm,
    const std::string& variant,
    int iterations,
    double bailoutSq,
    const std::string& colorMap,
    const std::string& colorMode,
    double cyclesPerOctave,
    double depthOctaves,
    const std::string& precisionMode
) {
    if (!lnMapCoordinateMatches(saved, "centerRe", "centerReStr", centerReStr, centerRe)) return "centerRe";
    if (!lnMapCoordinateMatches(saved, "centerIm", "centerImStr", centerImStr, centerIm)) return "centerIm";
    if (!saved.contains("julia") || !saved["julia"].is_boolean() || saved["julia"].get<bool>() != julia) return "julia";
    if (julia && !lnMapNumberMatches(saved, "juliaRe", juliaRe)) return "juliaRe";
    if (julia && !lnMapNumberMatches(saved, "juliaIm", juliaIm)) return "juliaIm";
    if (saved.value("variant", std::string()) != variant) return "variant";
    if (!saved.contains("iterations") || !saved["iterations"].is_number_integer() ||
        saved["iterations"].get<int>() != iterations) return "iterations";
    if (!lnMapNumberMatches(saved, "bailoutSq", bailoutSq)) return "bailoutSq";
    if (saved.value("colorMap", std::string()) != colorMap) return "colorMap";
    if (saved.value("lnMapColorMode", std::string()) != colorMode) return "lnMapColorMode";
    if (!lnMapNumberMatches(saved, "lnMapCyclesPerOctave", cyclesPerOctave)) return "lnMapCyclesPerOctave";
    if (!lnMapNumberMatches(saved, "depthOctaves", depthOctaves)) return "depthOctaves";
    if (saved.value("precisionMode", std::string()) != precisionMode) return "precisionMode";
    return {};
}

inline bool lnMapIntegerMatches(const ReuseJson& saved, const char* key, int requested) {
    return saved.contains(key) && saved[key].is_number_integer() &&
           saved[key].get<int>() == requested;
}

inline std::string lnMapGenerationIdentityMismatch(
    const ReuseJson& saved,
    const LnMapGenerationIdentity& requested
) {
    if (!lnMapNumberMatches(saved, "bailout", requested.bailout)) return "bailout";
    if (!lnMapNumberMatches(saved, "lnMapExtraOctaves", requested.extraOctaves)) return "lnMapExtraOctaves";
    if (saved.value("lnMapEngineRequest", std::string()) != requested.engineRequest) return "lnMapEngine";
    if (saved.value("lnMapScalarRequest", std::string()) != requested.scalarRequest) return "lnMapScalar";
    if (!lnMapNumberMatches(saved, "lnMapFastFp32DepthOctaves", requested.fastFp32DepthOctaves)) {
        return "lnMapFastFp32DepthOctaves";
    }
    if (!lnMapNumberMatches(saved, "lnMapFastFp64DepthOctaves", requested.fastFp64DepthOctaves)) {
        return "lnMapFastFp64DepthOctaves";
    }
    if (!saved.contains("lnMapFastValidate") || !saved["lnMapFastValidate"].is_boolean() ||
        saved["lnMapFastValidate"].get<bool>() != requested.fastValidate) {
        return "lnMapFastValidate";
    }
    if (!lnMapNumberMatches(
            saved, "lnMapFastValidationBandOctaves", requested.fastValidationBandOctaves)) {
        return "lnMapFastValidationBandOctaves";
    }
    if (!lnMapIntegerMatches(
            saved, "lnMapFastValidationSampleRows", requested.fastValidationSampleRows)) {
        return "lnMapFastValidationSampleRows";
    }
    if (!lnMapIntegerMatches(
            saved, "lnMapFastValidationSampleCols", requested.fastValidationSampleCols)) {
        return "lnMapFastValidationSampleCols";
    }
    if (!lnMapNumberMatches(
            saved, "lnMapFastValidationMaxMismatchRatio", requested.fastValidationMaxMismatchRatio)) {
        return "lnMapFastValidationMaxMismatchRatio";
    }
    if (!lnMapIntegerMatches(
            saved, "lnMapFastValidationMaxP99IterDelta", requested.fastValidationMaxP99IterDelta)) {
        return "lnMapFastValidationMaxP99IterDelta";
    }
    if (!lnMapNumberMatches(
            saved, "lnMapFastValidationMaxMeanColorDelta", requested.fastValidationMaxMeanColorDelta)) {
        return "lnMapFastValidationMaxMeanColorDelta";
    }
    return {};
}

inline void writeLnMapGenerationIdentity(
    ReuseJson& sidecar,
    const LnMapGenerationIdentity& identity
) {
    sidecar["bailout"] = identity.bailout;
    sidecar["lnMapExtraOctaves"] = identity.extraOctaves;
    sidecar["lnMapEngineRequest"] = identity.engineRequest;
    sidecar["lnMapScalarRequest"] = identity.scalarRequest;
    sidecar["lnMapFastFp32DepthOctaves"] = identity.fastFp32DepthOctaves;
    sidecar["lnMapFastFp64DepthOctaves"] = identity.fastFp64DepthOctaves;
    sidecar["lnMapFastValidate"] = identity.fastValidate;
    sidecar["lnMapFastValidationBandOctaves"] = identity.fastValidationBandOctaves;
    sidecar["lnMapFastValidationSampleRows"] = identity.fastValidationSampleRows;
    sidecar["lnMapFastValidationSampleCols"] = identity.fastValidationSampleCols;
    sidecar["lnMapFastValidationMaxMismatchRatio"] = identity.fastValidationMaxMismatchRatio;
    sidecar["lnMapFastValidationMaxP99IterDelta"] = identity.fastValidationMaxP99IterDelta;
    sidecar["lnMapFastValidationMaxMeanColorDelta"] = identity.fastValidationMaxMeanColorDelta;
}

} // namespace fsd::detail
