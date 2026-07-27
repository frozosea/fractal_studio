#include "compute/color_program_json.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

using fsd::compute::parse_color_program_json;
using Json = nlohmann::json;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
    const Json base = {
        {"schemaVersion", 1}, {"type", "gradient"},
        {"interpolation", "rgb"}, {"wrap", "clamp"},
        {"cycles", 1.0}, {"phase", 0.0},
        {"interiorColor", "#010203"}, {"invalidColor", "#ff00ff"},
        {"stops", Json::array({
            Json{{"at", 0.0}, {"color", "#000000"}},
            Json{{"at", 0.5}, {"color", "#ff0000"}},
            Json{{"at", 1.0}, {"color", "#ffffff"}},
        })},
    };
    const auto clamp = parse_color_program_json(base);
    std::uint8_t b = 0, g = 0, r = 0;
    clamp->colorize(0.25, b, g, r);
    require(r == 128 && g == 0 && b == 0, "RGB interpolation mismatch");
    clamp->colorizeInterior(b, g, r);
    require(r == 1 && g == 2 && b == 3, "interior color mismatch");

    Json repeatJson = base;
    repeatJson["wrap"] = "repeat";
    repeatJson["cycles"] = 2.0;
    const auto repeat = parse_color_program_json(repeatJson);
    repeat->colorize(0.75, b, g, r); // 1.5 wraps to 0.5
    require(r == 255 && g == 0 && b == 0, "repeat wrapping mismatch");

    Json mirrorJson = repeatJson;
    mirrorJson["wrap"] = "mirror";
    const auto mirror = parse_color_program_json(mirrorJson);
    mirror->colorize(0.75, b, g, r); // 1.5 mirrors to 0.5
    require(r == 255 && g == 0 && b == 0, "mirror wrapping mismatch");

    bool rejected = false;
    try {
        Json invalid = base;
        invalid["stops"][1]["color"] = "red";
        (void)parse_color_program_json(invalid);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "invalid hexadecimal color was accepted");
    return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
