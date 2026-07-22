#include "compute/orbit_program.hpp"
#include "compute/orbit_program_json.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace fsd::compute;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(Cx<double> a, Cx<double> b, double tolerance = 1e-12) {
    return std::abs(a.re - b.re) <= tolerance && std::abs(a.im - b.im) <= tolerance;
}

void verify_dsl() {
    const auto builtin = OrbitProgram::builtin(Variant::Mandelbrot);
    const auto dsl = OrbitProgram::formula("z^2+c");
    const Cx<double> z{0.25, -0.75};
    const Cx<double> c{-0.4, 0.2};
    require(close(builtin->step(z, c, 7), dsl->step(z, c, 7)),
            "z^2+c must match builtin Mandelbrot");
    require(dsl->escape_analysis().status == EscapeCertification::Unverified,
            "arbitrary DSL must not receive a finite escape certificate");

    const auto constants = OrbitProgram::formula(
        "z + gain*c + n + i + pi - e",
        {{"gain", {2.0, 0.0}}});
    const auto value = constants->step({1.0, 2.0}, {3.0, 4.0}, 5);
    require(std::isfinite(value.re) && std::isfinite(value.im),
            "DSL constants, n and parameters must evaluate");

    const auto hashA = OrbitProgram::formula(
        "a*z+b", {{"b", {2.0, 0.0}}, {"a", {1.0, 0.0}}})->hash();
    const auto hashB = OrbitProgram::formula(
        "a*z+b", {{"a", {1.0, 0.0}}, {"b", {2.0, 0.0}}})->hash();
    require(hashA == hashB, "parameter ordering must not change the formula hash");

    const auto realParameter = parse_orbit_program_json(nlohmann::json::parse(R"({
      "type":"formula","formula":{"type":"dsl","source":"gain*z","parameters":{"gain":2}}
    })"));
    const auto complexParameter = parse_orbit_program_json(nlohmann::json::parse(R"({
      "type":"formula","formula":{"type":"dsl","source":"gain*z","parameters":{"gain":[2,0]}}
    })"));
    require(realParameter->hash() != complexParameter->hash(),
            "declared real/complex parameter type must participate in canonical hash");
    require(realParameter->canonical().find("binary:complex") != std::string::npos,
            "typed AST must promote real times complex to complex");
    const auto realResult = OrbitProgram::formula("abs(z)+real(c)");
    require(realResult->canonical().find("binary:real") != std::string::npos,
            "typed AST must preserve real-valued function output");

    bool rejected = false;
    try { (void)OrbitProgram::formula("system(z)"); }
    catch (const FormulaCompileError& error) {
        rejected = error.code() == "UNKNOWN_FUNCTION" && error.position() == 0;
    }
    require(rejected, "unknown native-looking function must be rejected with position");
}

void verify_sequence() {
    const auto mandelbrot = OrbitProgram::builtin(Variant::Mandelbrot);
    const auto ship = OrbitProgram::builtin(Variant::Boat);
    const auto sequence = OrbitProgram::sequence({{1, mandelbrot}, {1, ship}}, true);
    const Cx<double> c{-0.7, 0.3};
    Cx<double> actual{0.0, 0.0};
    Cx<double> expected{0.0, 0.0};
    for (int n = 0; n < 8; ++n) {
        actual = sequence->step(actual, c, n);
        expected = (n % 2 == 0 ? mandelbrot : ship)->step(expected, c, n);
        require(close(actual, expected), "Mandelbrot/Burning Ship sequence schedule mismatch");
    }
    require(sequence->escape_analysis().has_finite_radius() &&
            std::abs(sequence->escape_analysis().certified_radius - 2.0) < 1e-12,
            "quadratic sequence must compose the common radius-2 certificate");

    const auto json = nlohmann::json::parse(R"({
      "type":"sequence","repeat":true,"steps":[
        {"span":1,"program":{"type":"formula","formula":{"type":"builtin","id":"mandelbrot"}}},
        {"span":1,"program":{"type":"formula","formula":{"type":"builtin","id":"burning_ship"}}}
      ]})");
    const auto parsed = parse_orbit_program_json(json);
    require(parsed->hash() == sequence->hash(), "JSON Orbit Program hash must be canonical");
    require(escape_analysis_json(parsed->escape_analysis())["certifiedRadius"] == 2.0,
            "escape analysis JSON must expose the certified radius");
}

void verify_output_blend_counterexample() {
    // This is deliberately expressed as DSL rather than enabling the future
    // output_blend IR node. Averaging complex outputs destroys the individual
    // formulas' radius-2 implication through cancellation, so no child
    // certificate may be interpolated or inherited.
    const auto blend = OrbitProgram::formula(
        "0.5*((z^2+c)+((abs(real(z))+i*abs(imag(z)))^2+c))");
    require(blend->escape_analysis().status == EscapeCertification::Unverified,
            "50% Mandelbrot/Burning Ship output blend must be unverified");
    const auto analysis = escape_analysis_json(blend->escape_analysis());
    require(analysis["certifiedRadius"].is_null(),
            "50% Mandelbrot/Burning Ship output blend must have infinite/null threshold");
}

void verify_limits() {
    bool rejected = false;
    try { (void)OrbitProgram::formula(std::string(4097, 'z')); }
    catch (const FormulaCompileError& error) { rejected = error.code() == "FORMULA_TOO_LONG"; }
    require(rejected, "formula source limit must be enforced");

    rejected = false;
    try {
        (void)parse_orbit_program_json(nlohmann::json{
            {"type", "sequence"}, {"repeat", false}, {"steps", nlohmann::json::array()}});
    } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "non-repeating sequence must be rejected in v1");
}

} // namespace

int main() {
    try {
        verify_dsl();
        verify_sequence();
        verify_output_blend_counterexample();
        verify_limits();
        std::cout << "orbit_program_smoke: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "orbit_program_smoke: " << error.what() << '\n';
        return 1;
    }
}
