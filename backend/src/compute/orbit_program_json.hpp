#pragma once

#include "orbit_program.hpp"
#include "../third_party/nlohmann/json.hpp"

#include <memory>

namespace fsd::compute {

// Parse the version-1 declarative Orbit Program JSON. The parser rejects
// unknown node/formula types instead of silently substituting Mandelbrot.
std::shared_ptr<const OrbitProgram> parse_orbit_program_json(
    const nlohmann::json& value);

nlohmann::json escape_analysis_json(const EscapeAnalysis& analysis);

} // namespace fsd::compute
