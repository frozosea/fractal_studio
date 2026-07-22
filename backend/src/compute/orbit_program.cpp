#include "orbit_program.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <sstream>
#include <utility>

namespace fsd::compute {
namespace {

constexpr std::size_t MAX_SOURCE_BYTES = 4096;
constexpr std::size_t MAX_AST_NODES = 256;
constexpr std::size_t MAX_AST_DEPTH = 32;
constexpr std::size_t MAX_PARAMETERS = 16;
constexpr std::size_t MAX_SEQUENCE_STEPS = 64;
constexpr std::size_t MAX_PROGRAM_OPERATIONS = 512;

enum class TokenKind { End, Number, Identifier, Plus, Minus, Star, Slash, Caret, LParen, RParen, Comma };

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;
    double number = 0.0;
    std::size_t position = 0;
};

class Lexer {
public:
    explicit Lexer(const std::string& source) : source_(source) {}

    Token next() {
        while (position_ < source_.size() && std::isspace(static_cast<unsigned char>(source_[position_]))) {
            ++position_;
        }
        if (position_ >= source_.size()) return {TokenKind::End, "", 0.0, position_};
        const std::size_t start = position_;
        const char ch = source_[position_++];
        switch (ch) {
            case '+': return {TokenKind::Plus, "+", 0.0, start};
            case '-': return {TokenKind::Minus, "-", 0.0, start};
            case '*': return {TokenKind::Star, "*", 0.0, start};
            case '/': return {TokenKind::Slash, "/", 0.0, start};
            case '^': return {TokenKind::Caret, "^", 0.0, start};
            case '(': return {TokenKind::LParen, "(", 0.0, start};
            case ')': return {TokenKind::RParen, ")", 0.0, start};
            case ',': return {TokenKind::Comma, ",", 0.0, start};
            default: break;
        }
        if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.') {
            const char* begin = source_.c_str() + start;
            char* end = nullptr;
            const double value = std::strtod(begin, &end);
            if (end == begin || !std::isfinite(value)) {
                throw FormulaCompileError("INVALID_NUMBER", start, "invalid or non-finite number");
            }
            position_ = static_cast<std::size_t>(end - source_.c_str());
            return {TokenKind::Number, source_.substr(start, position_ - start), value, start};
        }
        if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
            while (position_ < source_.size()) {
                const unsigned char next = static_cast<unsigned char>(source_[position_]);
                if (!std::isalnum(next) && next != '_') break;
                ++position_;
            }
            return {TokenKind::Identifier, source_.substr(start, position_ - start), 0.0, start};
        }
        throw FormulaCompileError("INVALID_CHARACTER", start, "character is not part of the formula DSL");
    }

private:
    const std::string& source_;
    std::size_t position_ = 0;
};

enum class NodeKind { Number, Variable, Unary, Binary, Call };

struct Node {
    NodeKind kind = NodeKind::Number;
    std::string text;
    double number = 0.0;
    std::size_t position = 0;
    std::vector<std::unique_ptr<Node>> children;
};

class Parser {
public:
    Parser(const std::string& source, const std::set<std::string>& parameters)
        : lexer_(source), parameters_(parameters) {
        current_ = lexer_.next();
    }

    std::unique_ptr<Node> parse() {
        auto root = expression(0, 1);
        if (current_.kind != TokenKind::End) fail("UNEXPECTED_TOKEN", current_, "unexpected token after formula");
        return root;
    }

    std::size_t node_count() const noexcept { return node_count_; }

private:
    static int leftBindingPower(TokenKind kind) {
        if (kind == TokenKind::Plus || kind == TokenKind::Minus) return 10;
        if (kind == TokenKind::Star || kind == TokenKind::Slash) return 20;
        if (kind == TokenKind::Caret) return 30;
        return 0;
    }

    [[noreturn]] static void fail(const std::string& code, const Token& token, const std::string& message) {
        throw FormulaCompileError(code, token.position, message);
    }

    std::unique_ptr<Node> makeNode(NodeKind kind, const Token& token, std::size_t depth) {
        if (depth > MAX_AST_DEPTH) fail("FORMULA_TOO_DEEP", token, "formula nesting exceeds 32 levels");
        if (++node_count_ > MAX_AST_NODES) fail("FORMULA_TOO_COMPLEX", token, "formula exceeds 256 AST nodes");
        auto node = std::make_unique<Node>();
        node->kind = kind;
        node->text = token.text;
        node->number = token.number;
        node->position = token.position;
        return node;
    }

    void advance() { current_ = lexer_.next(); }

    std::unique_ptr<Node> expression(int rightBindingPower, std::size_t depth) {
        Token token = current_;
        advance();
        auto left = prefix(token, depth);
        while (rightBindingPower < leftBindingPower(current_.kind)) {
            token = current_;
            advance();
            left = infix(token, std::move(left), depth);
        }
        return left;
    }

    std::unique_ptr<Node> prefix(const Token& token, std::size_t depth) {
        if (token.kind == TokenKind::Number) return makeNode(NodeKind::Number, token, depth);
        if (token.kind == TokenKind::Plus || token.kind == TokenKind::Minus) {
            auto node = makeNode(NodeKind::Unary, token, depth);
            node->children.push_back(expression(25, depth + 1));
            return node;
        }
        if (token.kind == TokenKind::LParen) {
            auto node = expression(0, depth + 1);
            if (current_.kind != TokenKind::RParen) fail("EXPECTED_RPAREN", current_, "expected ')'");
            advance();
            return node;
        }
        if (token.kind != TokenKind::Identifier) fail("EXPECTED_EXPRESSION", token, "expected expression");
        if (current_.kind != TokenKind::LParen) {
            static const std::set<std::string> variables = {"z", "c", "n", "i", "pi", "e"};
            if (!variables.count(token.text) && !parameters_.count(token.text)) {
                fail("UNKNOWN_IDENTIFIER", token, "unknown variable or parameter: " + token.text);
            }
            return makeNode(NodeKind::Variable, token, depth);
        }

        static const std::map<std::string, int> functions = {
            {"sin", 1}, {"cos", 1}, {"tan", 1}, {"exp", 1}, {"log", 1},
            {"sqrt", 1}, {"abs", 1}, {"conj", 1}, {"sinh", 1}, {"cosh", 1},
            {"tanh", 1}, {"real", 1}, {"imag", 1}, {"pow", 2},
        };
        const auto found = functions.find(token.text);
        if (found == functions.end()) fail("UNKNOWN_FUNCTION", token, "unknown function: " + token.text);
        advance();
        auto node = makeNode(NodeKind::Call, token, depth);
        if (current_.kind != TokenKind::RParen) {
            for (;;) {
                node->children.push_back(expression(0, depth + 1));
                if (current_.kind != TokenKind::Comma) break;
                advance();
            }
        }
        if (current_.kind != TokenKind::RParen) fail("EXPECTED_RPAREN", current_, "expected ')' after arguments");
        advance();
        if (static_cast<int>(node->children.size()) != found->second) {
            fail("INVALID_ARITY", token, token.text + " expects " + std::to_string(found->second) + " argument(s)");
        }
        return node;
    }

    std::unique_ptr<Node> infix(const Token& token, std::unique_ptr<Node> left, std::size_t depth) {
        auto node = makeNode(NodeKind::Binary, token, depth);
        node->children.push_back(std::move(left));
        const int binding = leftBindingPower(token.kind);
        node->children.push_back(expression(token.kind == TokenKind::Caret ? binding - 1 : binding, depth + 1));
        return node;
    }

    Lexer lexer_;
    const std::set<std::string>& parameters_;
    Token current_;
    std::size_t node_count_ = 0;
};

enum class Op {
    Number, Z, C, N, Parameter, Neg, Add, Sub, Mul, Div, Pow,
    Sin, Cos, Tan, Exp, Log, Sqrt, Abs, Conj, Sinh, Cosh, Tanh, Real, Imag,
};

struct Instruction {
    Op op = Op::Number;
    std::complex<double> value{};
    std::size_t index = 0;
};

using ParameterIndex = std::map<std::string, std::size_t>;

void compileNode(const Node& node, const ParameterIndex& parameters,
                 std::vector<Instruction>& code, std::ostringstream& canonical) {
    if (node.kind == NodeKind::Number) {
        code.push_back({Op::Number, {node.number, 0.0}, 0});
        canonical << "num(" << std::setprecision(17) << node.number << ')';
        return;
    }
    if (node.kind == NodeKind::Variable) {
        canonical << "var(" << node.text << ')';
        if (node.text == "z") code.push_back({Op::Z});
        else if (node.text == "c") code.push_back({Op::C});
        else if (node.text == "n") code.push_back({Op::N});
        else if (node.text == "i") code.push_back({Op::Number, {0.0, 1.0}, 0});
        else if (node.text == "pi") code.push_back({Op::Number, {std::numbers::pi, 0.0}, 0});
        else if (node.text == "e") code.push_back({Op::Number, {std::numbers::e, 0.0}, 0});
        else code.push_back({Op::Parameter, {}, parameters.at(node.text)});
        return;
    }
    canonical << (node.kind == NodeKind::Call ? "call(" : node.kind == NodeKind::Unary ? "unary(" : "binary(")
              << node.text;
    for (const auto& child : node.children) {
        canonical << ',';
        compileNode(*child, parameters, code, canonical);
    }
    canonical << ')';
    if (node.kind == NodeKind::Unary) {
        if (node.text == "-") code.push_back({Op::Neg});
        return;
    }
    if (node.kind == NodeKind::Binary) {
        if (node.text == "+") code.push_back({Op::Add});
        else if (node.text == "-") code.push_back({Op::Sub});
        else if (node.text == "*") code.push_back({Op::Mul});
        else if (node.text == "/") code.push_back({Op::Div});
        else code.push_back({Op::Pow});
        return;
    }
    static const std::map<std::string, Op> functions = {
        {"sin", Op::Sin}, {"cos", Op::Cos}, {"tan", Op::Tan}, {"exp", Op::Exp},
        {"log", Op::Log}, {"pow", Op::Pow}, {"sqrt", Op::Sqrt}, {"abs", Op::Abs},
        {"conj", Op::Conj}, {"sinh", Op::Sinh}, {"cosh", Op::Cosh}, {"tanh", Op::Tanh},
        {"real", Op::Real}, {"imag", Op::Imag},
    };
    code.push_back({functions.at(node.text)});
}

std::complex<double> evaluate(const std::vector<Instruction>& code,
                              const std::vector<std::complex<double>>& parameters,
                              std::complex<double> z, std::complex<double> c, int iteration) {
    // The bytecode limit makes a fixed stack possible. Avoiding a heap
    // allocation here matters because this evaluator runs once per pixel per
    // iteration.
    std::array<std::complex<double>, MAX_PROGRAM_OPERATIONS> stack{};
    std::size_t top = 0;
    auto push = [&](std::complex<double> value) {
        if (top >= stack.size()) throw std::runtime_error("formula bytecode stack overflow");
        stack[top++] = value;
    };
    auto pop = [&]() {
        if (top == 0) throw std::runtime_error("formula bytecode stack underflow");
        return stack[--top];
    };
    for (const Instruction& instruction : code) {
        switch (instruction.op) {
            case Op::Number: push(instruction.value); break;
            case Op::Z: push(z); break;
            case Op::C: push(c); break;
            case Op::N: push({static_cast<double>(iteration), 0.0}); break;
            case Op::Parameter: push(parameters.at(instruction.index)); break;
            case Op::Neg: stack[top - 1] = -stack[top - 1]; break;
            case Op::Add: { const auto b = pop(); stack[top - 1] += b; break; }
            case Op::Sub: { const auto b = pop(); stack[top - 1] -= b; break; }
            case Op::Mul: { const auto b = pop(); stack[top - 1] *= b; break; }
            case Op::Div: { const auto b = pop(); stack[top - 1] /= b; break; }
            case Op::Pow: { const auto b = pop(); stack[top - 1] = std::pow(stack[top - 1], b); break; }
            case Op::Sin: stack[top - 1] = std::sin(stack[top - 1]); break;
            case Op::Cos: stack[top - 1] = std::cos(stack[top - 1]); break;
            case Op::Tan: stack[top - 1] = std::tan(stack[top - 1]); break;
            case Op::Exp: stack[top - 1] = std::exp(stack[top - 1]); break;
            case Op::Log: stack[top - 1] = std::log(stack[top - 1]); break;
            case Op::Sqrt: stack[top - 1] = std::sqrt(stack[top - 1]); break;
            case Op::Abs: stack[top - 1] = {std::abs(stack[top - 1]), 0.0}; break;
            case Op::Conj: stack[top - 1] = std::conj(stack[top - 1]); break;
            case Op::Sinh: stack[top - 1] = std::sinh(stack[top - 1]); break;
            case Op::Cosh: stack[top - 1] = std::cosh(stack[top - 1]); break;
            case Op::Tanh: stack[top - 1] = std::tanh(stack[top - 1]); break;
            case Op::Real: stack[top - 1] = {stack[top - 1].real(), 0.0}; break;
            case Op::Imag: stack[top - 1] = {stack[top - 1].imag(), 0.0}; break;
        }
    }
    if (top != 1) throw std::runtime_error("invalid formula bytecode stack");
    return stack[0];
}

std::string sha256(const std::string& value) {
    EVP_MD_CTX* raw = EVP_MD_CTX_new();
    if (!raw) throw std::runtime_error("failed to allocate formula hash context");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(raw, EVP_MD_CTX_free);
    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx.get(), value.data(), value.size()) != 1) {
        throw std::runtime_error("failed to hash formula");
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &length) != 1) {
        throw std::runtime_error("failed to finalize formula hash");
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < length; ++i) out << std::setw(2) << static_cast<int>(digest[i]);
    return out.str();
}

Cx<double> builtinStep(Variant variant, Cx<double> z, const Cx<double>& c) {
    switch (variant) {
#define FSD_ORBIT_BUILTIN(V) case Variant::V: return variant_step<Variant::V, double>(z, c)
        FSD_ORBIT_BUILTIN(Mandelbrot); FSD_ORBIT_BUILTIN(Tri); FSD_ORBIT_BUILTIN(Boat);
        FSD_ORBIT_BUILTIN(Duck); FSD_ORBIT_BUILTIN(Bell); FSD_ORBIT_BUILTIN(Fish);
        FSD_ORBIT_BUILTIN(Vase); FSD_ORBIT_BUILTIN(Bird); FSD_ORBIT_BUILTIN(Mask);
        FSD_ORBIT_BUILTIN(Ship); FSD_ORBIT_BUILTIN(SinZ); FSD_ORBIT_BUILTIN(CosZ);
        FSD_ORBIT_BUILTIN(ExpZ); FSD_ORBIT_BUILTIN(SinhZ); FSD_ORBIT_BUILTIN(CoshZ);
        FSD_ORBIT_BUILTIN(TanZ);
#undef FSD_ORBIT_BUILTIN
        case Variant::Custom: break;
    }
    throw std::runtime_error("unsupported builtin Orbit Program variant");
}

} // namespace

struct OrbitProgram::Impl {
    enum class Kind { Builtin, Formula, Sequence } kind = Kind::Builtin;
    Variant variant = Variant::Mandelbrot;
    std::vector<Instruction> code;
    std::vector<std::complex<double>> parameters;
    std::vector<OrbitSequenceStep> steps;
    int cycle_span = 0;
    EscapeAnalysis escape;
    std::string canonical;
    std::string hash;
    std::size_t operation_count = 0;
    std::size_t node_count = 1;
    std::size_t depth = 1;
};

FormulaCompileError::FormulaCompileError(
    std::string code, std::size_t position, std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)), position_(position) {}

OrbitProgram::OrbitProgram(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

std::shared_ptr<const OrbitProgram> OrbitProgram::builtin(Variant variant) {
    if (variant == Variant::Custom) throw std::runtime_error("custom is not a builtin Orbit Program formula");
    auto impl = std::make_shared<Impl>();
    impl->kind = Impl::Kind::Builtin;
    impl->variant = variant;
    impl->canonical = std::string("builtin:") + variant_name(variant);
    impl->hash = sha256(impl->canonical);
    impl->operation_count = 1;
    if (static_cast<int>(variant) >= 0 && static_cast<int>(variant) <= 9) {
        impl->escape = {EscapeCertification::CertifiedFinite, 2.0,
                        "quadratic Mandelbrot-family norm-preserving template"};
    } else {
        impl->escape = {EscapeCertification::Unverified, 0.0,
                        "transcendental builtin has no certified radial escape bound"};
    }
    return std::shared_ptr<const OrbitProgram>(new OrbitProgram(std::move(impl)));
}

std::shared_ptr<const OrbitProgram> OrbitProgram::formula(
    const std::string& source, const std::vector<OrbitParameter>& parameters) {
    if (source.empty()) throw FormulaCompileError("EMPTY_FORMULA", 0, "formula is empty");
    if (source.size() > MAX_SOURCE_BYTES) {
        throw FormulaCompileError("FORMULA_TOO_LONG", MAX_SOURCE_BYTES, "formula exceeds 4096 bytes");
    }
    if (parameters.size() > MAX_PARAMETERS) {
        throw FormulaCompileError("TOO_MANY_PARAMETERS", 0, "formula declares more than 16 parameters");
    }
    std::vector<OrbitParameter> sortedParameters = parameters;
    std::sort(sortedParameters.begin(), sortedParameters.end(),
              [](const OrbitParameter& a, const OrbitParameter& b) { return a.name < b.name; });
    std::set<std::string> names;
    ParameterIndex indexes;
    std::vector<std::complex<double>> values;
    std::ostringstream parameterCanonical;
    for (const OrbitParameter& parameter : sortedParameters) {
        if (parameter.name.empty() || !std::isalpha(static_cast<unsigned char>(parameter.name.front())) ||
            !std::all_of(parameter.name.begin(), parameter.name.end(), [](unsigned char ch) {
                return std::isalnum(ch) || ch == '_';
            })) {
            throw FormulaCompileError("INVALID_PARAMETER", 0, "invalid parameter name: " + parameter.name);
        }
        if (!names.insert(parameter.name).second) {
            throw FormulaCompileError("DUPLICATE_PARAMETER", 0, "duplicate parameter: " + parameter.name);
        }
        static const std::set<std::string> reserved = {"z", "c", "n", "i", "pi", "e"};
        if (reserved.count(parameter.name)) {
            throw FormulaCompileError("RESERVED_PARAMETER", 0,
                                      "parameter uses a reserved name: " + parameter.name);
        }
        if (!std::isfinite(parameter.value.re) || !std::isfinite(parameter.value.im)) {
            throw FormulaCompileError("INVALID_PARAMETER_VALUE", 0,
                                      "parameter value must be finite: " + parameter.name);
        }
        indexes[parameter.name] = values.size();
        values.emplace_back(parameter.value.re, parameter.value.im);
        parameterCanonical << parameter.name << '=' << std::setprecision(17)
                           << parameter.value.re << ',' << parameter.value.im << ';';
    }
    Parser parser(source, names);
    auto ast = parser.parse();
    auto impl = std::make_shared<Impl>();
    impl->kind = Impl::Kind::Formula;
    impl->parameters = std::move(values);
    std::ostringstream canonical;
    canonical << "dsl1{" << parameterCanonical.str() << "}:";
    compileNode(*ast, indexes, impl->code, canonical);
    if (impl->code.size() > MAX_PROGRAM_OPERATIONS) {
        throw FormulaCompileError("FORMULA_TOO_COMPLEX", 0, "formula exceeds bytecode operation budget");
    }
    impl->canonical = canonical.str();
    impl->hash = sha256(impl->canonical);
    impl->operation_count = impl->code.size();
    impl->node_count = parser.node_count();
    impl->escape = {EscapeCertification::Unverified, 0.0,
                    "arbitrary DSL formula has no certified radial escape proof"};
    return std::shared_ptr<const OrbitProgram>(new OrbitProgram(std::move(impl)));
}

std::shared_ptr<const OrbitProgram> OrbitProgram::sequence(
    const std::vector<OrbitSequenceStep>& steps, bool repeat) {
    if (!repeat) throw std::runtime_error("Orbit Program v1 sequences must repeat");
    if (steps.empty() || steps.size() > MAX_SEQUENCE_STEPS) {
        throw std::runtime_error("Orbit Program sequence needs 1..64 steps");
    }
    auto impl = std::make_shared<Impl>();
    impl->kind = Impl::Kind::Sequence;
    impl->steps = steps;
    impl->escape = {EscapeCertification::CertifiedFinite, 2.0,
                    "all sequence steps share the certified quadratic family bound"};
    std::ostringstream canonical;
    canonical << "sequence1(";
    for (const OrbitSequenceStep& step : steps) {
        if (!step.program || step.span < 1 || step.span > 1000000) {
            throw std::runtime_error("Orbit Program sequence span must be in 1..1000000");
        }
        if (impl->cycle_span > 1000000 - step.span) {
            throw std::runtime_error("Orbit Program sequence cycle exceeds 1000000 iterations");
        }
        impl->cycle_span += step.span;
        impl->operation_count = std::max(impl->operation_count, step.program->operation_count());
        if (impl->node_count > MAX_AST_NODES - step.program->node_count()) {
            throw std::runtime_error("Orbit Program exceeds 256 total nodes");
        }
        impl->node_count += step.program->node_count();
        impl->depth = std::max(impl->depth, 1 + step.program->depth());
        if (impl->depth > MAX_AST_DEPTH) {
            throw std::runtime_error("Orbit Program nesting exceeds 32 levels");
        }
        canonical << step.span << ':' << step.program->canonical() << ';';
        if (!step.program->escape_analysis().has_finite_radius() ||
            std::abs(step.program->escape_analysis().certified_radius - 2.0) > 1e-12) {
            impl->escape = {EscapeCertification::Unverified, 0.0,
                            "at least one sequence step lacks the common quadratic escape certificate"};
        }
    }
    canonical << ')';
    impl->canonical = canonical.str();
    impl->hash = sha256(impl->canonical);
    return std::shared_ptr<const OrbitProgram>(new OrbitProgram(std::move(impl)));
}

Cx<double> OrbitProgram::step(Cx<double> z, const Cx<double>& c, int iteration) const {
    if (impl_->kind == Impl::Kind::Builtin) return builtinStep(impl_->variant, z, c);
    if (impl_->kind == Impl::Kind::Formula) {
        const auto result = evaluate(impl_->code, impl_->parameters,
                                     {z.re, z.im}, {c.re, c.im}, iteration);
        return {result.real(), result.imag()};
    }
    int offset = iteration % impl_->cycle_span;
    if (offset < 0) offset += impl_->cycle_span;
    for (const OrbitSequenceStep& sequenceStep : impl_->steps) {
        if (offset < sequenceStep.span) return sequenceStep.program->step(z, c, iteration);
        offset -= sequenceStep.span;
    }
    throw std::runtime_error("invalid Orbit Program sequence state");
}

const EscapeAnalysis& OrbitProgram::escape_analysis() const noexcept { return impl_->escape; }
const std::string& OrbitProgram::canonical() const noexcept { return impl_->canonical; }
const std::string& OrbitProgram::hash() const noexcept { return impl_->hash; }
std::size_t OrbitProgram::operation_count() const noexcept { return impl_->operation_count; }
std::size_t OrbitProgram::node_count() const noexcept { return impl_->node_count; }
std::size_t OrbitProgram::depth() const noexcept { return impl_->depth; }

const char* escape_certification_name(EscapeCertification status) noexcept {
    switch (status) {
        case EscapeCertification::CertifiedFinite: return "certified_finite";
        case EscapeCertification::NoFiniteBound: return "no_finite_bound";
        case EscapeCertification::Unverified: return "unverified";
    }
    return "unverified";
}

} // namespace fsd::compute
