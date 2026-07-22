#include "compute/hs/heightfield_mesh.hpp"
#include "compute/orbit_program.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using namespace fsd::compute;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void requireFieldsClose(const std::vector<double>& a, const std::vector<double>& b) {
    require(a.size() == b.size(), "HS field size mismatch");
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::abs(a[i] - b[i]) > 1e-12) {
            throw std::runtime_error("single-formula Orbit HS differs from builtin HS");
        }
    }
}

} // namespace

int main() {
    try {
        hs::HsMeshParams legacy;
        legacy.resolution = 24;
        legacy.iterations = 40;
        legacy.variant = Variant::Mandelbrot;
        legacy.metric = Metric::MinAbs;
        std::vector<double> legacyField;
        hs::computeHsField(legacy, legacyField);

        hs::HsMeshParams formula = legacy;
        formula.orbit_program = OrbitProgram::builtin(Variant::Mandelbrot);
        std::vector<double> formulaField;
        hs::computeHsField(formula, formulaField);
        requireFieldsClose(legacyField, formulaField);

        hs::HsMeshParams sequence = legacy;
        sequence.metric = Metric::Escape;
        sequence.orbit_program = OrbitProgram::sequence({
            {1, OrbitProgram::builtin(Variant::Mandelbrot)},
            {1, OrbitProgram::builtin(Variant::Boat)},
        });
        std::vector<double> first;
        std::vector<double> second;
        hs::computeHsField(sequence, first);
        hs::computeHsField(sequence, second);
        require(first == second, "HS Orbit sequence must be deterministic");
        require(std::any_of(first.begin(), first.end(), [](double value) { return value < 1.0; }),
                "certified HS sequence should contain escaped samples");

        hs::HsMeshParams strict = legacy;
        strict.center_re = 100.0;
        strict.center_im = 0.0;
        strict.scale = 1e-6;
        strict.resolution = 8;
        strict.iterations = 5;
        strict.metric = Metric::Escape;
        strict.orbit_program = OrbitProgram::formula("c");
        std::vector<double> strictField;
        hs::computeHsField(strict, strictField);
        require(std::all_of(strictField.begin(), strictField.end(),
                            [](double value) { return value == 1.0; }),
                "unverified HS formula must not use a finite escape threshold");

        std::cout << "hs_orbit_smoke: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hs_orbit_smoke: " << error.what() << '\n';
        return 1;
    }
}
