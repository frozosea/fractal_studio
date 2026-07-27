#pragma once

#include "color_program.hpp"
#include "../third_party/nlohmann/json.hpp"

#include <memory>

namespace fsd::compute {

std::shared_ptr<const ColorProgram> parse_color_program_json(
    const nlohmann::json& value);

} // namespace fsd::compute
