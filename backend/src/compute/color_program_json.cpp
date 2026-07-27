#include "color_program_json.hpp"

#include <cmath>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fsd::compute {
namespace {

int hexDigit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return 10 + value - 'a';
    if (value >= 'A' && value <= 'F') return 10 + value - 'A';
    return -1;
}

ProgramColor parseColor(const nlohmann::json& value, const char* field) {
    if (!value.is_string()) {
        throw std::runtime_error(std::string(field) + " must be #RRGGBB");
    }
    const std::string text = value.get<std::string>();
    if (text.size() != 7 || text[0] != '#') {
        throw std::runtime_error(std::string(field) + " must be #RRGGBB");
    }
    int bytes[3]{};
    for (int index = 0; index < 3; ++index) {
        const int high = hexDigit(text[1 + index * 2]);
        const int low = hexDigit(text[2 + index * 2]);
        if (high < 0 || low < 0) {
            throw std::runtime_error(std::string(field) + " must be #RRGGBB");
        }
        bytes[index] = high * 16 + low;
    }
    return {
        static_cast<std::uint8_t>(bytes[0]),
        static_cast<std::uint8_t>(bytes[1]),
        static_cast<std::uint8_t>(bytes[2]),
    };
}

void rejectUnknownFields(const nlohmann::json& value,
                         const std::set<std::string>& allowed,
                         const char* objectName) {
    for (const auto& [name, ignored] : value.items()) {
        (void)ignored;
        if (!allowed.contains(name)) {
            throw std::runtime_error(std::string(objectName) +
                                     " contains unknown field: " + name);
        }
    }
}

} // namespace

std::shared_ptr<const ColorProgram> parse_color_program_json(
    const nlohmann::json& value) {
    if (!value.is_object()) throw std::runtime_error("colorProgram must be an object");
    rejectUnknownFields(value, {
        "schemaVersion", "type", "interpolation", "wrap", "cycles", "phase",
        "interiorColor", "invalidColor", "stops",
    }, "colorProgram");
    if (value.value("schemaVersion", 0) != 1)
        throw std::runtime_error("colorProgram.schemaVersion must be 1");
    if (value.value("type", std::string()) != "gradient")
        throw std::runtime_error("colorProgram.type must be gradient");
    if (value.value("interpolation", std::string("rgb")) != "rgb")
        throw std::runtime_error("colorProgram.interpolation must be rgb");

    ColorWrap wrap = ColorWrap::Clamp;
    const std::string wrapName = value.value("wrap", std::string("clamp"));
    if (wrapName == "repeat") wrap = ColorWrap::Repeat;
    else if (wrapName == "mirror") wrap = ColorWrap::Mirror;
    else if (wrapName != "clamp")
        throw std::runtime_error("colorProgram.wrap must be clamp, repeat, or mirror");

    const double cycles = value.value("cycles", 1.0);
    const double phase = value.value("phase", 0.0);
    if (!(cycles > 0.0) || cycles > 256.0 || !std::isfinite(cycles))
        throw std::runtime_error("colorProgram.cycles must be finite and in (0,256]");
    if (!std::isfinite(phase))
        throw std::runtime_error("colorProgram.phase must be finite");

    const ProgramColor interior = value.contains("interiorColor")
        ? parseColor(value["interiorColor"], "colorProgram.interiorColor")
        : ProgramColor{255, 255, 255};
    const ProgramColor invalid = value.contains("invalidColor")
        ? parseColor(value["invalidColor"], "colorProgram.invalidColor")
        : ProgramColor{255, 0, 255};

    if (!value.contains("stops") || !value["stops"].is_array() ||
        value["stops"].size() < 2 || value["stops"].size() > 16) {
        throw std::runtime_error("colorProgram.stops must contain 2..16 entries");
    }
    std::vector<ProgramColorStop> stops;
    stops.reserve(value["stops"].size());
    double previous = -1.0;
    for (const auto& stop : value["stops"]) {
        if (!stop.is_object()) throw std::runtime_error("color stop must be an object");
        rejectUnknownFields(stop, {"at", "color"}, "color stop");
        if (!stop.contains("at") || !stop["at"].is_number() ||
            !stop.contains("color")) {
            throw std::runtime_error("color stop requires numeric at and #RRGGBB color");
        }
        const double at = stop["at"].get<double>();
        if (!std::isfinite(at) || at < 0.0 || at > 1.0 || at <= previous)
            throw std::runtime_error("color stop positions must be finite and strictly increasing in [0,1]");
        stops.push_back({at, parseColor(stop["color"], "color stop color")});
        previous = at;
    }
    if (stops.front().at != 0.0 || stops.back().at != 1.0)
        throw std::runtime_error("colorProgram stops must start at 0 and end at 1");

    return std::make_shared<const ColorProgram>(
        std::move(stops), wrap, cycles, phase, interior, invalid);
}

} // namespace fsd::compute
