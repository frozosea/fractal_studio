#include "color_program.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace fsd::compute {
namespace {

void writeBgr(ProgramColor color, std::uint8_t& b, std::uint8_t& g,
              std::uint8_t& r) noexcept {
    r = color.r;
    g = color.g;
    b = color.b;
}

double wrapped(double value, ColorWrap wrap) noexcept {
    if (wrap == ColorWrap::Clamp) return std::clamp(value, 0.0, 1.0);
    if (wrap == ColorWrap::Repeat) {
        const double result = value - std::floor(value);
        return result < 0.0 ? result + 1.0 : result;
    }
    double result = std::fmod(value, 2.0);
    if (result < 0.0) result += 2.0;
    return result <= 1.0 ? result : 2.0 - result;
}

std::uint8_t mixChannel(std::uint8_t left, std::uint8_t right,
                        double amount) noexcept {
    const double mixed = static_cast<double>(left) * (1.0 - amount) +
        static_cast<double>(right) * amount;
    return static_cast<std::uint8_t>(std::clamp(
        static_cast<int>(std::lround(mixed)), 0, 255));
}

} // namespace

ColorProgram::ColorProgram(std::vector<ProgramColorStop> stops, ColorWrap wrap,
                           double cycles, double phase, ProgramColor interior,
                           ProgramColor invalid)
    : stops_(std::move(stops)), wrap_(wrap), cycles_(cycles), phase_(phase),
      interior_(interior), invalid_(invalid) {}

void ColorProgram::colorize(double input, std::uint8_t& b, std::uint8_t& g,
                            std::uint8_t& r) const noexcept {
    if (!std::isfinite(input) || stops_.size() < 2) {
        colorizeInvalid(b, g, r);
        return;
    }
    const double value = wrapped(input * cycles_ + phase_, wrap_);
    if (value <= stops_.front().at) {
        writeBgr(stops_.front().color, b, g, r);
        return;
    }
    for (std::size_t index = 1; index < stops_.size(); ++index) {
        if (value <= stops_[index].at) {
            const auto& left = stops_[index - 1];
            const auto& right = stops_[index];
            const double span = std::max(1e-12, right.at - left.at);
            const double amount = std::clamp((value - left.at) / span, 0.0, 1.0);
            writeBgr({
                mixChannel(left.color.r, right.color.r, amount),
                mixChannel(left.color.g, right.color.g, amount),
                mixChannel(left.color.b, right.color.b, amount),
            }, b, g, r);
            return;
        }
    }
    writeBgr(stops_.back().color, b, g, r);
}

void ColorProgram::colorizeInterior(std::uint8_t& b, std::uint8_t& g,
                                    std::uint8_t& r) const noexcept {
    writeBgr(interior_, b, g, r);
}

void ColorProgram::colorizeInvalid(std::uint8_t& b, std::uint8_t& g,
                                   std::uint8_t& r) const noexcept {
    writeBgr(invalid_, b, g, r);
}

} // namespace fsd::compute
