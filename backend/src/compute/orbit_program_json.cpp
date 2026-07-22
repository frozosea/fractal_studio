#include "orbit_program_json.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace fsd::compute {
namespace {

constexpr std::size_t MAX_JSON_NODES = 256;
constexpr std::size_t MAX_JSON_DEPTH = 32;

OrbitParameter parseParameterValue(const nlohmann::json& value, const std::string& name) {
    double re = 0.0;
    double im = 0.0;
    OrbitParameter::Type type = OrbitParameter::Type::Complex;
    if (value.is_number()) {
        re = value.get<double>();
        type = OrbitParameter::Type::Real;
    } else if (value.is_array() && value.size() == 2 &&
               value[0].is_number() && value[1].is_number()) {
        re = value[0].get<double>();
        im = value[1].get<double>();
    } else if (value.is_object() && value.contains("re") && value.contains("im") &&
               value["re"].is_number() && value["im"].is_number()) {
        re = value["re"].get<double>();
        im = value["im"].get<double>();
    } else {
        throw std::runtime_error("Orbit parameter '" + name +
                                 "' must be a number, [re, im], or {re, im}");
    }
    if (!std::isfinite(re) || !std::isfinite(im)) {
        throw std::runtime_error("Orbit parameter '" + name + "' must be finite");
    }
    return {name, {re, im}, type};
}

std::shared_ptr<const OrbitProgram> parseNode(
    const nlohmann::json& value, std::size_t depth, std::size_t& nodes) {
    if (!value.is_object()) throw std::runtime_error("Orbit Program node must be an object");
    if (depth > MAX_JSON_DEPTH) throw std::runtime_error("Orbit Program nesting exceeds 32 levels");
    if (++nodes > MAX_JSON_NODES) throw std::runtime_error("Orbit Program exceeds 256 nodes");

    const std::string type = value.value("type", std::string());
    if (type == "formula") {
        if (!value.contains("formula") || !value["formula"].is_object()) {
            throw std::runtime_error("formula node requires a formula object");
        }
        const auto& formula = value["formula"];
        const std::string formulaType = formula.value("type", std::string());
        if (formulaType == "builtin") {
            const std::string id = formula.value("id", std::string());
            Variant variant;
            if (id.empty() || !variant_from_name(id.c_str(), variant) || variant == Variant::Custom) {
                throw std::runtime_error("unknown Orbit builtin formula: " + id);
            }
            return OrbitProgram::builtin(variant);
        }
        if (formulaType == "dsl") {
            const std::string source = formula.value("source", std::string());
            std::vector<OrbitParameter> parameters;
            if (formula.contains("parameters")) {
                if (!formula["parameters"].is_object()) {
                    throw std::runtime_error("DSL parameters must be an object");
                }
                for (const auto& [name, parameter] : formula["parameters"].items()) {
                    parameters.push_back(parseParameterValue(parameter, name));
                }
            }
            return OrbitProgram::formula(source, parameters);
        }
        throw std::runtime_error("unsupported Orbit formula type: " + formulaType);
    }

    if (type == "sequence") {
        if (!value.value("repeat", true)) {
            throw std::runtime_error("Orbit Program v1 sequences must repeat");
        }
        if (!value.contains("steps") || !value["steps"].is_array()) {
            throw std::runtime_error("sequence node requires a steps array");
        }
        std::vector<OrbitSequenceStep> steps;
        for (const auto& step : value["steps"]) {
            if (!step.is_object() || !step.contains("program")) {
                throw std::runtime_error("sequence step requires span and program");
            }
            if (!step.contains("span") || !step["span"].is_number_integer()) {
                throw std::runtime_error("sequence span must be a positive integer");
            }
            const long long span = step["span"].get<long long>();
            if (span < 1 || span > 1000000) {
                throw std::runtime_error("sequence span must be in 1..1000000");
            }
            steps.push_back({static_cast<int>(span), parseNode(step["program"], depth + 1, nodes)});
        }
        return OrbitProgram::sequence(steps, true);
    }

    throw std::runtime_error("unsupported Orbit Program node type: " + type);
}

} // namespace

std::shared_ptr<const OrbitProgram> parse_orbit_program_json(const nlohmann::json& value) {
    std::size_t nodes = 0;
    return parseNode(value, 1, nodes);
}

nlohmann::json escape_analysis_json(const EscapeAnalysis& analysis) {
    nlohmann::json radius = nullptr;
    if (analysis.has_finite_radius()) radius = analysis.certified_radius;
    return {
        {"status", escape_certification_name(analysis.status)},
        {"certifiedRadius", std::move(radius)},
        {"reason", analysis.reason},
    };
}

} // namespace fsd::compute
