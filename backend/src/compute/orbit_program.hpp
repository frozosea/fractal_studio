#pragma once

#include "complex.hpp"
#include "variants.hpp"

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fsd::compute {

enum class EscapeCertification {
    CertifiedFinite,
    NoFiniteBound,
    Unverified,
};

struct EscapeAnalysis {
    EscapeCertification status = EscapeCertification::Unverified;
    double certified_radius = 0.0;
    std::string reason;

    bool has_finite_radius() const noexcept {
        return status == EscapeCertification::CertifiedFinite && certified_radius > 0.0;
    }
};

struct OrbitParameter {
    std::string name;
    Cx<double> value;
};

class FormulaCompileError : public std::runtime_error {
public:
    FormulaCompileError(std::string code, std::size_t position, std::string message);
    const std::string& code() const noexcept { return code_; }
    std::size_t position() const noexcept { return position_; }

private:
    std::string code_;
    std::size_t position_ = 0;
};

class OrbitProgram;

struct OrbitSequenceStep {
    int span = 1;
    std::shared_ptr<const OrbitProgram> program;
};

class OrbitProgram {
public:
    static std::shared_ptr<const OrbitProgram> builtin(Variant variant);
    static std::shared_ptr<const OrbitProgram> formula(
        const std::string& source,
        const std::vector<OrbitParameter>& parameters = {});
    static std::shared_ptr<const OrbitProgram> sequence(
        const std::vector<OrbitSequenceStep>& steps,
        bool repeat = true);

    Cx<double> step(Cx<double> z, const Cx<double>& c, int iteration) const;
    const EscapeAnalysis& escape_analysis() const noexcept;
    const std::string& canonical() const noexcept;
    const std::string& hash() const noexcept;
    std::size_t operation_count() const noexcept;
    std::size_t node_count() const noexcept;
    std::size_t depth() const noexcept;

private:
    struct Impl;
    explicit OrbitProgram(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

const char* escape_certification_name(EscapeCertification status) noexcept;

} // namespace fsd::compute
