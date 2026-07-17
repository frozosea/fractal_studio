// routes_variants.cpp
//
// Dynamic formula compile + custom variant registry.
//
// POST /api/variants/compile  — validate formula, compile .so, dlopen, persist
// GET  /api/variants           — list built-in + loaded custom variants
// POST /api/variants/delete    — unload + delete a custom variant by hash
//
// Security model: formulae are validated via character allowlist + identifier
// whitelist before any C++ is generated. This is a local dev tool; the attack
// surface is the local user only.

#include "routes.hpp"
#include "routes_common.hpp"

#include "../include/db.hpp"
#include "../compute/variants.hpp"
#include "../compute/map_kernel.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fsd {

namespace {

// ─── Global registry ─────────────────────────────────────────────────────────

std::mutex g_mu;

// hash → dlopen handle. A custom render can hold a shared lease after the
// registry entry is deleted, keeping its function pointer valid until that
// render actually finishes.
std::map<std::string, std::shared_ptr<void>> g_libs;

// hash → resolved step_fn pointer
std::map<std::string, compute::CustomStepFn> g_fns;

// Flag: have we loaded existing variants from the DB this session?
bool g_dbLoaded = false;

// ─── Formula hash (FNV-1a 64-bit over formula + bailout bytes) ───────────────

std::string formulaHash(const std::string& formula, double bailout) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : formula) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    const auto* bp = reinterpret_cast<const unsigned char*>(&bailout);
    for (int i = 0; i < static_cast<int>(sizeof(double)); i++) {
        h ^= bp[i];
        h *= 1099511628211ULL;
    }
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << h;
    return ss.str();
}

// ─── Formula validation ───────────────────────────────────────────────────────
//
// Allowed characters: z c + - * / ^ ( ) . 0-9 a-z _ whitespace
// Allowed identifiers: z c sin cos tan exp log pow sqrt abs conj sinh cosh tanh real imag
// Disallowed: anything else, including ; { } # \ " '

static const std::set<std::string> ALLOWED_IDS = {
    "z", "c",
    "sin", "cos", "tan", "exp", "log", "pow", "sqrt",
    "abs", "conj", "sinh", "cosh", "tanh", "real", "imag",
};

bool validateFormula(const std::string& formula, std::string& err) {
    // Character allowlist
    for (unsigned char ch : formula) {
        const bool ok = std::isalnum(ch) || ch == '+' || ch == '-' || ch == '*' || ch == '/'
                     || ch == '(' || ch == ')' || ch == '.' || ch == '_' || ch == '^'
                     || ch == ' ' || ch == '\t';
        if (!ok) {
            err = std::string("disallowed character: '") + static_cast<char>(ch) + "'";
            return false;
        }
    }

    // Extract identifiers and check against whitelist
    for (size_t i = 0; i < formula.size(); ) {
        const char ch = formula[i];
        if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
            // Collect identifier
            size_t j = i;
            while (j < formula.size() &&
                   (std::isalnum(static_cast<unsigned char>(formula[j])) || formula[j] == '_'))
                ++j;
            const std::string id = formula.substr(i, j - i);
            if (ALLOWED_IDS.find(id) == ALLOWED_IDS.end()) {
                err = "disallowed identifier: '" + id + "'";
                return false;
            }
            i = j;
        } else {
            ++i;
        }
    }

    // Must not be empty after trimming
    std::string trimmed = formula;
    trimmed.erase(0, trimmed.find_first_not_of(" \t"));
    trimmed.erase(trimmed.find_last_not_of(" \t") + 1);
    if (trimmed.empty()) { err = "empty formula"; return false; }

    return true;
}

// ─── .so path derivation ─────────────────────────────────────────────────────

std::filesystem::path soPath(const std::filesystem::path& repoRoot, const std::string& hash) {
    const auto dir = repoRoot / "runs" / "custom_variants";
    std::filesystem::create_directories(dir);
    return dir / ("fsd_custom_" + hash + ".so");
}

std::filesystem::path cppPath(const std::string& hash) {
    return std::filesystem::temp_directory_path() / ("fsd_custom_" + hash + ".cpp");
}

std::string formulaKey(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char ch : s) {
        if (!std::isspace(ch)) out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

double multibrotBailoutForPower(int power) {
    if (power < 2) return 2.0;
    return std::pow(2.0, 1.0 / static_cast<double>(power - 1));
}

double multibrotBailoutSqForPower(int power) {
    if (power < 2) return 4.0;
    return std::pow(2.0, 2.0 / static_cast<double>(power - 1));
}

int inferFormulaPower(const std::string& formula) {
    const std::string s = formulaKey(formula);
    if (s == "z*z+c") return 2;
    if (s == "z*z*z+c") return 3;

    const std::string prefix = "z^";
    const std::string suffix = "+c";
    if (s.rfind(prefix, 0) == 0 && s.size() > prefix.size() + suffix.size()
        && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0) {
        const std::string exp = s.substr(prefix.size(), s.size() - prefix.size() - suffix.size());
        if (!exp.empty() && std::all_of(exp.begin(), exp.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
            return std::stoi(exp);
        }
    }

    const std::string powPrefix = "pow(z,";
    const std::string powSuffix = ")+c";
    if (s.rfind(powPrefix, 0) == 0 && s.size() > powPrefix.size() + powSuffix.size()
        && s.compare(s.size() - powSuffix.size(), powSuffix.size(), powSuffix) == 0) {
        const std::string exp = s.substr(powPrefix.size(), s.size() - powPrefix.size() - powSuffix.size());
        if (!exp.empty() && std::all_of(exp.begin(), exp.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
            return std::stoi(exp);
        }
    }

    return 0;
}

double inferFormulaBailout(const std::string& formula) {
    const int power = inferFormulaPower(formula);
    if (power >= 2) return multibrotBailoutForPower(power);
    return 2.0;
}

double inferFormulaBailoutSq(const std::string& formula) {
    const int power = inferFormulaPower(formula);
    if (power >= 2) return multibrotBailoutSqForPower(power);
    return 4.0;
}

double effectiveCustomBailout(const std::string& formula, double storedBailout) {
    const int power = inferFormulaPower(formula);
    if (power >= 2 && std::abs(storedBailout - 4.0) < 1e-12) {
        return multibrotBailoutForPower(power);
    }
    return storedBailout;
}

double effectiveCustomBailoutSq(const std::string& formula, double storedBailout) {
    const int power = inferFormulaPower(formula);
    if (power >= 2) {
        const double inferred = multibrotBailoutForPower(power);
        if (std::abs(storedBailout - 4.0) < 1e-12 ||
            std::abs(storedBailout - inferred) < 1e-12) {
            return multibrotBailoutSqForPower(power);
        }
    }
    return storedBailout * storedBailout;
}

// ─── Compile a formula into a shared library ──────────────────────────────────

// Expand ^ to pow() for user convenience. Very simple: replace a^b with pow(a,b)
// This is a best-effort pass: handles simple integer exponents and variable^2.
// We only process it if the formula contains '^' at all.
std::string expandCaret(const std::string& formula) {
    if (formula.find('^') == std::string::npos) return formula;
    // Replace x^y with pow(x,y) using a simple left-to-right scan.
    // This does NOT handle nested parens correctly for complex cases,
    // but covers the common use cases like z^2, z^3, c^2.
    std::string result;
    result.reserve(formula.size() + 16);
    for (size_t i = 0; i < formula.size(); i++) {
        if (formula[i] == '^') {
            // Find start of left operand (scan back to find atom)
            size_t lend = result.size();
            size_t lstart = lend;
            // skip trailing spaces
            while (lstart > 0 && result[lstart - 1] == ' ') --lstart;
            if (lstart > 0 && result[lstart - 1] == ')') {
                // find matching (
                int depth = 1;
                --lstart;
                while (lstart > 0 && depth > 0) {
                    --lstart;
                    if (result[lstart] == ')') ++depth;
                    else if (result[lstart] == '(') --depth;
                }
            } else {
                // identifier or number
                while (lstart > 0 &&
                       (std::isalnum(static_cast<unsigned char>(result[lstart - 1]))
                        || result[lstart - 1] == '_' || result[lstart - 1] == '.'))
                    --lstart;
            }
            const std::string lhs = result.substr(lstart, lend - lstart);
            result.erase(lstart);

            // Find right operand (scan forward)
            size_t ri = i + 1;
            while (ri < formula.size() && formula[ri] == ' ') ++ri;
            size_t rstart = ri;
            if (ri < formula.size() && formula[ri] == '(') {
                int depth = 1;
                ++ri;
                while (ri < formula.size() && depth > 0) {
                    if (formula[ri] == '(') ++depth;
                    else if (formula[ri] == ')') --depth;
                    ++ri;
                }
            } else {
                while (ri < formula.size() &&
                       (std::isalnum(static_cast<unsigned char>(formula[ri]))
                        || formula[ri] == '_' || formula[ri] == '.'))
                    ++ri;
            }
            const std::string rhs = formula.substr(rstart, ri - rstart);
            i = ri - 1;  // -1 because outer loop will ++i

            result += "pow(" + lhs + "," + rhs + ")";
        } else {
            result += formula[i];
        }
    }
    return result;
}

bool compileSo(const std::string& formula, double bailout,
               const std::string& hash,
               const std::filesystem::path& soOut,
               std::string& errMsg) {
    (void)bailout;

    // Generate C++ source
    const std::string expanded = expandCaret(formula);
    const auto src = cppPath(hash);
    {
        std::ofstream f(src);
        if (!f) { errMsg = "cannot write temp source file"; return false; }
        f << "#include <complex>\n"
          << "#include <cmath>\n"
          << "using C = std::complex<double>;\n"
          << "using std::sin; using std::cos; using std::tan;\n"
          << "using std::exp; using std::log; using std::pow;\n"
          << "using std::sqrt; using std::abs; using std::conj;\n"
          << "using std::sinh; using std::cosh; using std::tanh;\n"
          << "using std::real; using std::imag;\n"
          << "extern \"C\" void step_fn(double zr, double zi, double cr, double ci,\n"
          << "                         double* or_, double* oi) {\n"
          << "    C z(zr, zi), c(cr, ci);\n"
          << "    C result = " << expanded << ";\n"
          << "    *or_ = result.real();\n"
          << "    *oi  = result.imag();\n"
          << "}\n";
    }

    // Compile
    const std::string cmd =
        "g++ -O2 -shared -fPIC -std=c++17 -o "
        + soOut.string()
        + " "
        + src.string()
        + " 2>&1";

    char buf[4096] = {};
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) { errMsg = "popen failed"; return false; }
    size_t nread = ::fread(buf, 1, sizeof(buf) - 1, pipe);
    (void)nread;
    const int status = ::pclose(pipe);

    // Clean up temp source
    std::filesystem::remove(src);

    if (status != 0) {
        errMsg = std::string(buf);
        if (errMsg.empty()) errMsg = "compilation failed (exit " + std::to_string(status) + ")";
        return false;
    }
    return true;
}

// ─── dlopen / dlsym ──────────────────────────────────────────────────────────

bool loadLibLocked(const std::string& hash, const std::filesystem::path& so,
                   std::string& errMsg) {
    // Already loaded?
    if (g_fns.count(hash)) return true;

    if (!std::filesystem::exists(so)) {
        errMsg = "shared library not found: " + so.string();
        return false;
    }

    void* handle = ::dlopen(so.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        errMsg = std::string("dlopen failed: ") + ::dlerror();
        return false;
    }

    // Clear dlerror before dlsym
    ::dlerror();
    void* sym = ::dlsym(handle, "step_fn");
    const char* derr = ::dlerror();
    if (derr || !sym) {
        ::dlclose(handle);
        errMsg = std::string("dlsym failed: ") + (derr ? derr : "null symbol");
        return false;
    }

    g_libs[hash] = std::shared_ptr<void>(handle, [](void* h) {
        if (h) ::dlclose(h);
    });
    // POSIX C standard mandates this cast pattern for function pointers via dlsym.
    compute::CustomStepFn fn;
    std::memcpy(&fn, &sym, sizeof(fn));
    g_fns[hash] = fn;
    return true;
}

// Ensure all variants persisted in DB are loaded (called lazily on first use).
void ensureDbLoadedLocked(const std::filesystem::path& repoRoot) {
    if (g_dbLoaded) return;
    g_dbLoaded = true;

    Db db(repoRoot / "fractal_studio.db");
    db.ensureSchema();
    try {
        const auto rows = db.listCustomVariants();
        for (const auto& r : rows) {
            if (g_fns.count(r.hash)) continue;
            std::string err;
            if (!loadLibLocked(r.hash, r.soPath, err)) {
                // .so may be missing (e.g. /tmp cleared). Recompile silently.
                std::string cerr;
                if (compileSo(r.formula, r.bailout, r.hash, r.soPath, cerr)) {
                    loadLibLocked(r.hash, r.soPath, err);
                }
                // If recompile also fails, skip — user will need to recompile manually.
            }
        }
    } catch (...) {
        // DB not yet populated or other error — ignore.
    }
}

} // namespace

// ─── Public lookup (called from routes_map.cpp) ───────────────────────────────

CustomVariantLease acquireCustomVariantLease(
    const std::filesystem::path& repoRoot,
    const std::string& hash
) {
    std::lock_guard<std::mutex> lock(g_mu);
    ensureDbLoadedLocked(repoRoot);

    Db db(repoRoot / "fractal_studio.db");
    CustomVariantRecord rec;
    if (!db.getCustomVariantByHash(hash, rec)) return {};

    if (!g_fns.count(hash)) {
        std::string err;
        if (!loadLibLocked(hash, rec.soPath, err)) {
            std::string compileError;
            if (!compileSo(rec.formula, rec.bailout, hash, rec.soPath, compileError) ||
                !loadLibLocked(hash, rec.soPath, err)) {
                return {};
            }
        }
    }

    const auto fnIt = g_fns.find(hash);
    const auto libIt = g_libs.find(hash);
    if (fnIt == g_fns.end() || libIt == g_libs.end()) return {};

    void* raw = nullptr;
    std::memcpy(&raw, &fnIt->second, sizeof(raw));
    return {
        libIt->second,
        raw,
        effectiveCustomBailout(rec.formula, rec.bailout),
        effectiveCustomBailoutSq(rec.formula, rec.bailout),
    };
}

void* lookupCustomFn(const std::filesystem::path& repoRoot, const std::string& hash) {
    std::lock_guard<std::mutex> lock(g_mu);
    ensureDbLoadedLocked(repoRoot);

    const auto it = g_fns.find(hash);
    if (it != g_fns.end()) {
        void* p;
        std::memcpy(&p, &it->second, sizeof(p));
        return p;
    }

    // Try loading from DB on-demand (handles the case where a hash arrived
    // from the client but ensureDbLoaded didn't pick it up due to a race).
    Db db(repoRoot / "fractal_studio.db");
    CustomVariantRecord rec;
    if (!db.getCustomVariantByHash(hash, rec)) return nullptr;

    std::string err;
    if (!loadLibLocked(hash, rec.soPath, err)) {
        // Try recompile
        std::string cerr;
        if (!compileSo(rec.formula, rec.bailout, hash, rec.soPath, cerr)) return nullptr;
        if (!loadLibLocked(hash, rec.soPath, err)) return nullptr;
    }

    const auto it2 = g_fns.find(hash);
    if (it2 == g_fns.end()) return nullptr;
    void* p;
    std::memcpy(&p, &it2->second, sizeof(p));
    return p;
}

double lookupCustomBailout(const std::filesystem::path& repoRoot, const std::string& hash) {
    std::lock_guard<std::mutex> lock(g_mu);
    ensureDbLoadedLocked(repoRoot);

    Db db(repoRoot / "fractal_studio.db");
    CustomVariantRecord rec;
    if (!db.getCustomVariantByHash(hash, rec)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return effectiveCustomBailout(rec.formula, rec.bailout);
}

double lookupCustomBailoutSq(const std::filesystem::path& repoRoot, const std::string& hash) {
    std::lock_guard<std::mutex> lock(g_mu);
    ensureDbLoadedLocked(repoRoot);

    Db db(repoRoot / "fractal_studio.db");
    CustomVariantRecord rec;
    if (!db.getCustomVariantByHash(hash, rec)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return effectiveCustomBailoutSq(rec.formula, rec.bailout);
}

// ─── Route: POST /api/variants/compile ───────────────────────────────────────

std::string variantCompileRoute(const std::filesystem::path& repoRoot, const std::string& body) {
    const Json j = parseJsonBody(body);

    const std::string formula = j.value("formula", std::string(""));
    const std::string name    = j.value("name",    std::string("custom"));
    const bool explicitBailout = j.contains("bailout") && !j["bailout"].is_null();
    const double bailout      = explicitBailout
        ? j.value("bailout", 2.0)
        : inferFormulaBailout(formula);
    const double bailoutSq    = explicitBailout ? bailout * bailout : inferFormulaBailoutSq(formula);

    if (formula.empty()) throw std::runtime_error("formula is required");
    if (bailout <= 0.0 || bailout > 1e6) throw std::runtime_error("bailout must be in (0, 1e6]");

    // Validate
    std::string validErr;
    if (!validateFormula(formula, validErr)) {
        Json err = {{"ok", false}, {"error", validErr}};
        return err.dump();
    }

    const std::string hash = formulaHash(formula, bailout);
    const std::string variantId = "custom:" + hash;

    // Check if already compiled
    {
        std::lock_guard<std::mutex> lock(g_mu);
        ensureDbLoadedLocked(repoRoot);
        if (g_fns.count(hash)) {
            Json resp = {
                {"ok", true}, {"variantId", variantId}, {"name", name},
                {"bailout", bailout}, {"bailoutSq", bailoutSq},
                {"cached", true}
            };
            return resp.dump();
        }
    }

    // Compile
    const auto so = soPath(repoRoot, hash);
    std::string compileErr;
    if (!compileSo(formula, bailout, hash, so, compileErr)) {
        Json err = {{"ok", false}, {"error", "compile error: " + compileErr}};
        return err.dump();
    }

    // Load
    {
        std::lock_guard<std::mutex> lock(g_mu);
        std::string loadErr;
        if (!loadLibLocked(hash, so, loadErr)) {
            Json err = {{"ok", false}, {"error", "dlopen error: " + loadErr}};
            return err.dump();
        }
    }

    // Persist to DB
    {
        Db db(repoRoot / "fractal_studio.db");
        CustomVariantRecord rec;
        rec.hash      = hash;
        rec.name      = name;
        rec.formula   = formula;
        rec.bailout   = bailout;
        rec.soPath    = so.string();
        rec.createdAt = nowIso8601();
        db.insertCustomVariant(rec);
    }

    Json resp = {
        {"ok",        true},
        {"variantId", variantId},
        {"name",      name},
        {"hash",      hash},
        {"bailout",   bailout},
        {"bailoutSq", bailoutSq},
        {"cached",    false},
    };
    return resp.dump();
}

// ─── Route: GET /api/variants ─────────────────────────────────────────────────

std::string variantListRoute(const std::filesystem::path& repoRoot) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        ensureDbLoadedLocked(repoRoot);
    }

    // Built-in variants
    Json builtin = Json::array();
    static const std::pair<const char*, const char*> BUILTIN[] = {
        {"mandelbrot",  "Mandelbrot"},
        {"tricorn",     "Tricorn / Mandelbar"},
        {"burning_ship","Burning Ship"},
        {"celtic",      "Perpendicular Burning Ship"},
        {"heart",       "Perpendicular Mandelbrot"},
        {"buffalo",     "Celtic"},
        {"perp_buffalo","Mandelbar Celtic"},
        {"celtic_ship", "Buffalo"},
        {"mandelceltic","Perpendicular Buffalo"},
        {"perp_ship",   "Perpendicular Celtic"},
        {"sin_z",       "sin(z)+c"},
        {"cos_z",       "cos(z)+c"},
        {"exp_z",       "exp(z)+c"},
        {"sinh_z",      "sinh(z)+c"},
        {"cosh_z",      "cosh(z)+c"},
        {"tan_z",       "tan(z)+c"},
    };
    for (const auto& [id, label] : BUILTIN) {
        builtin.push_back({{"variantId", id}, {"name", label}, {"builtin", true}});
    }

    // Custom variants from DB
    Db db(repoRoot / "fractal_studio.db");
    db.ensureSchema();
    const auto rows = db.listCustomVariants();
    Json custom = Json::array();
    for (const auto& r : rows) {
        custom.push_back({
            {"variantId",  "custom:" + r.hash},
            {"name",       r.name},
            {"formula",    r.formula},
            {"bailout",    effectiveCustomBailout(r.formula, r.bailout)},
            {"bailoutSq",  effectiveCustomBailoutSq(r.formula, r.bailout)},
            {"createdAt",  r.createdAt},
            {"loaded",     g_fns.count(r.hash) > 0},
        });
    }

    Json resp = {{"builtin", builtin}, {"custom", custom}};
    return resp.dump();
}

// ─── Route: POST /api/variants/delete ────────────────────────────────────────

std::string variantDeleteRoute(const std::filesystem::path& repoRoot, const std::string& body) {
    const Json j = parseJsonBody(body);
    const std::string variantId = j.value("variantId", std::string(""));
    if (variantId.rfind("custom:", 0) != 0) throw std::runtime_error("variantId must start with 'custom:'");
    const std::string hash = variantId.substr(7);

    // Unload from registry
    {
        std::lock_guard<std::mutex> lock(g_mu);
        const auto lit = g_libs.find(hash);
        if (lit != g_libs.end()) {
            g_libs.erase(lit);
        }
        g_fns.erase(hash);
    }

    // Delete .so file
    {
        Db db(repoRoot / "fractal_studio.db");
        CustomVariantRecord rec;
        if (db.getCustomVariantByHash(hash, rec)) {
            std::filesystem::remove(rec.soPath);
        }
        db.deleteCustomVariant(hash);
    }

    Json resp = {{"ok", true}};
    return resp.dump();
}

} // namespace fsd
