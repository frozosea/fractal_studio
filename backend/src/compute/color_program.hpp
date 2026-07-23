#pragma once

#include <cstdint>
#include <vector>

namespace fsd::compute {

struct ProgramColor {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

struct ProgramColorStop {
    double at = 0.0;
    ProgramColor color;
};

enum class ColorWrap {
    Clamp,
    Repeat,
    Mirror,
};

// Safe declarative presentation program. It contains no executable user code;
// per-pixel work is bounded by the 16-stop parser limit.
class ColorProgram {
public:
    ColorProgram(std::vector<ProgramColorStop> stops, ColorWrap wrap,
                 double cycles, double phase, ProgramColor interior,
                 ProgramColor invalid);

    void colorize(double input, std::uint8_t& b, std::uint8_t& g,
                  std::uint8_t& r) const noexcept;
    void colorizeInterior(std::uint8_t& b, std::uint8_t& g,
                          std::uint8_t& r) const noexcept;
    void colorizeInvalid(std::uint8_t& b, std::uint8_t& g,
                         std::uint8_t& r) const noexcept;

    const std::vector<ProgramColorStop>& stops() const noexcept { return stops_; }
    ColorWrap wrap() const noexcept { return wrap_; }
    double cycles() const noexcept { return cycles_; }
    double phase() const noexcept { return phase_; }
    ProgramColor interior() const noexcept { return interior_; }
    ProgramColor invalid() const noexcept { return invalid_; }

private:
    std::vector<ProgramColorStop> stops_;
    ColorWrap wrap_ = ColorWrap::Clamp;
    double cycles_ = 1.0;
    double phase_ = 0.0;
    ProgramColor interior_{255, 255, 255};
    ProgramColor invalid_{255, 0, 255};
};

} // namespace fsd::compute
