#include "special_points.hpp"

#include "parallel.hpp"
#include "variants.hpp"

#if defined(HAS_CUDA_KERNEL)
#  include "cuda/special_points.cuh"
#  define USE_CUDA_SPECIAL_POINTS 1
#else
#  define USE_CUDA_SPECIAL_POINTS 0
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace fsd::compute {
namespace {

using Z = std::complex<double>;

constexpr double PI = 3.14159265358979323846264338327950288;
constexpr int LOCAL_ADAPTIVE_NEWTON_MAX_ITER = 240;
constexpr int LOCAL_VIEWPORT_SEED_LIMIT = 160;
constexpr int LOCAL_PERIOD_BLOCK_MIN = 8;
constexpr int LOCAL_PERIOD_BLOCK_MAX = 128;
constexpr int LOCAL_CUDA_SEED_BATCH_MIN = 32;

struct Task {
    SpecialPointKind kind;
    int preperiod;
    int period;
    int expected;
};

struct SeedSolveOutcome {
    int seed_count = 0;
    int newton_success_count = 0;
    int rejected_count = 0;
    std::vector<SpecialPointResult> accepted;
    std::vector<SpecialPointResult> rejected_debug;
    bool has_fallback_candidate = false;
    SpecialPointResult fallback_candidate;
};

struct SearchStepResult {
    bool accepted = false;
    bool cancelled = false;
};

struct OrbitCell {
    long long x;
    long long y;

    bool operator==(const OrbitCell& other) const {
        return x == other.x && y == other.y;
    }
};

struct OrbitCellHash {
    size_t operator()(const OrbitCell& cell) const {
        const auto x = static_cast<unsigned long long>(cell.x);
        const auto y = static_cast<unsigned long long>(cell.y);
        return static_cast<size_t>((x * 0x9e3779b97f4a7c15ULL) ^ (y + 0xbf58476d1ce4e5b9ULL + (x << 6) + (x >> 2)));
    }
};

bool finite(Z z) {
    return std::isfinite(z.real()) && std::isfinite(z.imag());
}

int env_int(const char* name, int min_value, int max_value) {
    const char* raw = std::getenv(name);
    if (!raw || *raw == '\0') return 0;
    char* end = nullptr;
    const long parsed = std::strtol(raw, &end, 10);
    if (end == raw || parsed < min_value || parsed > max_value) return 0;
    return static_cast<int>(parsed);
}

int local_period_wave_size() {
    const int env_wave = env_int("FSD_SPECIAL_POINT_WAVE", 1, 4096);
    if (env_wave > 0) return env_wave;
    return std::clamp(
        default_render_threads(),
        LOCAL_PERIOD_BLOCK_MIN,
        LOCAL_PERIOD_BLOCK_MAX);
}

#if USE_CUDA_SPECIAL_POINTS
bool special_point_cuda_enabled() {
    const char* raw = std::getenv("FSD_SPECIAL_POINT_CUDA");
    if (!raw || *raw == '\0') return true;
    return !(raw[0] == '0' || raw[0] == 'f' || raw[0] == 'F' ||
             raw[0] == 'n' || raw[0] == 'N');
}

int special_point_cuda_seed_batch_min() {
    const int env_min = env_int("FSD_SPECIAL_POINT_CUDA_MIN", 1, 4096);
    return env_min > 0 ? env_min : LOCAL_CUDA_SEED_BATCH_MIN;
}
#endif

long long orbit_cell_coord(double value, double cell_size) {
    const double q = std::floor(value / cell_size);
    if (!std::isfinite(q)) return q < 0.0 ? std::numeric_limits<long long>::min() : std::numeric_limits<long long>::max();
    if (q <= static_cast<double>(std::numeric_limits<long long>::min())) return std::numeric_limits<long long>::min();
    if (q >= static_cast<double>(std::numeric_limits<long long>::max())) return std::numeric_limits<long long>::max();
    return static_cast<long long>(q);
}

OrbitCell orbit_cell(Z z, double cell_size) {
    return {orbit_cell_coord(z.real(), cell_size), orbit_cell_coord(z.imag(), cell_size)};
}

long long orbit_cell_offset(long long value, long long delta) {
    if (delta < 0 && value == std::numeric_limits<long long>::min()) return value;
    if (delta > 0 && value == std::numeric_limits<long long>::max()) return value;
    return value + delta;
}

Z normalize_root(Z z, double eps) {
    double re = z.real();
    double im = z.imag();
    if (std::abs(im) < eps) im = 0.0;
    if (std::abs(re) < eps) re = 0.0;
    return {re, im};
}

std::string format_id(SpecialPointKind kind, int preperiod, int period, Z c) {
    std::ostringstream ss;
    ss << (kind == SpecialPointKind::HyperbolicCenter ? "center" : "misiurewicz")
       << "-m" << preperiod << "-p" << period << "-";
    ss << std::fixed << std::setprecision(12) << c.real();
    if (c.imag() >= 0.0) ss << "+";
    ss << std::fixed << std::setprecision(12) << c.imag() << "i";
    return ss.str();
}

int mobius_center_count(int p) {
    int count = 1 << (p - 1);
    for (int d = 1; d < p; ++d) {
        if (p % d == 0) count -= expected_center_count(d);
    }
    return count;
}

double halton(unsigned long long index, int base) {
    double f = 1.0;
    double r = 0.0;
    while (index > 0) {
        f /= static_cast<double>(base);
        r += f * static_cast<double>(index % static_cast<unsigned long long>(base));
        index /= static_cast<unsigned long long>(base);
    }
    return r;
}

Z seed_for(unsigned long long index) {
    // Deterministic low-discrepancy sampling directly inside |c| <= 2.
    // Full enumeration uses this global disk; viewport-local search has its
    // own sampler around the current view.
    const double u = halton(index + 1, 2);
    const double v = halton(index + 1, 3);
    const double r = 2.0 * std::sqrt(u);
    const double theta = 2.0 * PI * v;
    return {r * std::cos(theta), r * std::sin(theta)};
}

double viewport_half_height(const SpecialPointSearchRequest& req) {
    return std::max(req.viewport.scale * 0.5, std::numeric_limits<double>::min());
}

double viewport_half_width(const SpecialPointSearchRequest& req) {
    const double aspect = static_cast<double>(std::max(1, req.viewport.width)) /
        static_cast<double>(std::max(1, req.viewport.height));
    return viewport_half_height(req) * aspect;
}

Z viewport_seed_at(const SpecialPointSearchRequest& req, double u, double v) {
    const double half_w = viewport_half_width(req);
    const double half_h = viewport_half_height(req);
    return {
        req.viewport.center_re + (u - 0.5) * 2.0 * half_w,
        req.viewport.center_im + (v - 0.5) * 2.0 * half_h,
    };
}

void push_unique_seed(std::vector<Z>& seeds, Z seed, double eps) {
    if (!finite(seed)) return;
    for (Z existing : seeds) {
        if (std::abs(existing - seed) <= eps) return;
    }
    seeds.push_back(seed);
}

int viewport_search_seed_limit(const SpecialPointSearchRequest& req) {
    const int budget = req.seed_budget > 0 ? req.seed_budget : 1;
    return std::max(1, std::min(budget, LOCAL_VIEWPORT_SEED_LIMIT));
}

std::vector<Z> viewport_search_seeds(const SpecialPointSearchRequest& req) {
    std::vector<Z> seeds;
    const int limit = viewport_search_seed_limit(req);
    seeds.reserve(static_cast<size_t>(limit));
    const double duplicate_eps = std::max(req.viewport.scale * 1e-9, 1e-16);

    push_unique_seed(seeds, {req.viewport.center_re, req.viewport.center_im}, duplicate_eps);

    static constexpr double fixed[][2] = {
        {0.50, 0.35}, {0.50, 0.65}, {0.35, 0.50}, {0.65, 0.50},
        {0.35, 0.35}, {0.65, 0.35}, {0.35, 0.65}, {0.65, 0.65},
        {0.50, 0.20}, {0.50, 0.80}, {0.20, 0.50}, {0.80, 0.50},
        {0.20, 0.20}, {0.80, 0.20}, {0.20, 0.80}, {0.80, 0.80},
        {0.12, 0.35}, {0.88, 0.35}, {0.12, 0.65}, {0.88, 0.65},
        {0.35, 0.12}, {0.65, 0.12}, {0.35, 0.88}, {0.65, 0.88},
    };
    for (const auto& uv : fixed) {
        if (static_cast<int>(seeds.size()) >= limit) break;
        push_unique_seed(seeds, viewport_seed_at(req, uv[0], uv[1]), duplicate_eps);
    }

    for (unsigned long long i = 0; static_cast<int>(seeds.size()) < limit; ++i) {
        const double u = halton(i + 1, 2);
        const double v = halton(i + 1, 3);
        push_unique_seed(seeds, viewport_seed_at(req, u, v), duplicate_eps);
    }

    return seeds;
}

bool same_root(const SpecialPointResult& a, const SpecialPointResult& b, double eps) {
    return a.kind == b.kind
        && a.preperiod == b.preperiod
        && a.period == b.period
        && std::abs(Z(a.re, a.im) - Z(b.re, b.im)) < eps;
}

template <Variant V>
Z variant_step_std(Z z, Z c) {
    Cx<double> zz{z.real(), z.imag()};
    Cx<double> cc{c.real(), c.imag()};
    const Cx<double> out = variant_step<V, double>(zz, cc);
    return {out.re, out.im};
}

Z variant_step_switch(Variant v, Z z, Z c) {
    switch (v) {
        case Variant::Mandelbrot: return variant_step_std<Variant::Mandelbrot>(z, c);
        case Variant::Tri:        return variant_step_std<Variant::Tri>(z, c);
        case Variant::Boat:       return variant_step_std<Variant::Boat>(z, c);
        case Variant::Duck:       return variant_step_std<Variant::Duck>(z, c);
        case Variant::Bell:       return variant_step_std<Variant::Bell>(z, c);
        case Variant::Fish:       return variant_step_std<Variant::Fish>(z, c);
        case Variant::Vase:       return variant_step_std<Variant::Vase>(z, c);
        case Variant::Bird:       return variant_step_std<Variant::Bird>(z, c);
        case Variant::Mask:       return variant_step_std<Variant::Mask>(z, c);
        case Variant::Ship:       return variant_step_std<Variant::Ship>(z, c);
        default:                  return variant_step_std<Variant::Mandelbrot>(z, c);
    }
}

std::string display_variant_name(Variant v) {
    switch (v) {
        case Variant::Mandelbrot: return "Mandelbrot";
        case Variant::Tri:        return "Tri";
        case Variant::Boat:       return "Boat";
        case Variant::Duck:       return "Duck";
        case Variant::Bell:       return "Bell";
        case Variant::Fish:       return "Fish";
        case Variant::Vase:       return "Vase";
        case Variant::Bird:       return "Bird";
        case Variant::Mask:       return "Mask";
        case Variant::Ship:       return "Ship";
        default:                  return "Unsupported";
    }
}

OrbitClassification classify_orbit_with_step(
    Z c,
    int max_iter,
    double eps,
    const std::function<Z(Z, Z)>& step
) {
    OrbitClassification out;
    out.orbit.reserve(static_cast<size_t>(max_iter) + 1);
    const double cell_size = std::max(eps, std::numeric_limits<double>::epsilon());
    std::unordered_map<OrbitCell, std::vector<int>, OrbitCellHash> cells;
    cells.reserve(static_cast<size_t>(max_iter) * 2 + 1);

    int best_i = std::numeric_limits<int>::max();
    int best_j = -1;
    double best_err = 0.0;
    auto insert_orbit_point = [&](int index, Z point) {
        cells[orbit_cell(point, cell_size)].push_back(index);
    };
    auto check_repeats = [&](int j, Z point) {
        const OrbitCell base = orbit_cell(point, cell_size);
        for (long long dy = -1; dy <= 1; ++dy) {
            for (long long dx = -1; dx <= 1; ++dx) {
                const OrbitCell key{orbit_cell_offset(base.x, dx), orbit_cell_offset(base.y, dy)};
                const auto it = cells.find(key);
                if (it == cells.end()) continue;
                for (int i : it->second) {
                    const double err = std::abs(point - out.orbit[static_cast<size_t>(i)]);
                    if (err < eps && (i < best_i || (i == best_i && (best_j < 0 || j < best_j)))) {
                        best_i = i;
                        best_j = j;
                        best_err = err;
                    }
                }
            }
        }
    };

    Z z{0.0, 0.0};
    out.orbit.push_back(z);
    insert_orbit_point(0, z);
    for (int n = 1; n <= max_iter; ++n) {
        z = step(z, c);
        if (!finite(z)) break;
        check_repeats(n, z);
        out.orbit.push_back(z);
        insert_orbit_point(n, z);
        if (best_i == 0) break;
    }

    if (best_i != std::numeric_limits<int>::max()) {
        out.found_repeat = true;
        out.preperiod = best_i;
        out.period = best_j - best_i;
        out.repeat_error = best_err;
        out.is_center = best_i == 0;
        out.is_misiurewicz = best_i > 0;
    }
    return out;
}

bool matches_request(
    const OrbitClassification& actual,
    SpecialPointKind kind,
    int requested_preperiod,
    int requested_period
) {
    if (kind == SpecialPointKind::HyperbolicCenter) {
        return actual.is_center && actual.period == requested_period;
    }
    return actual.is_misiurewicz
        && actual.preperiod == requested_preperiod
        && actual.period == requested_period;
}

std::string rejection_reason(
    const OrbitClassification& actual,
    SpecialPointKind kind,
    int requested_preperiod,
    int requested_period
) {
    if (!actual.found_repeat) return "no_repeat";
    if (kind == SpecialPointKind::HyperbolicCenter) {
        if (!actual.is_center) return "not_center";
        if (actual.period < requested_period) return "degenerated_period";
        return "wrong_period";
    }
    if (actual.is_center) return "center_not_misiurewicz";
    if (!actual.is_misiurewicz) return "not_misiurewicz";
    if (actual.preperiod < requested_preperiod) return "early_preperiod";
    if (actual.period < requested_period) return "degenerated_period";
    if (actual.preperiod != requested_preperiod) return "wrong_preperiod";
    return "wrong_period";
}

double newton_accept_eps(const SpecialPointEnumRequest& req) {
    // For high-period equations z_p(c)=0, double precision often bottoms out
    // above the requested Newton epsilon even when the orbit is well within the
    // later classification tolerance. Accept the root once it is comfortably
    // below that classifier threshold.
    return std::max(req.newton_eps, req.classify_eps * 0.1);
}

void push_unique_period(std::vector<int>& periods, int period) {
    if (period < 1) return;
    if (std::find(periods.begin(), periods.end(), period) == periods.end()) {
        periods.push_back(period);
    }
}

void append_period_family(
    std::vector<int>& candidates,
    int period,
    const SpecialPointSearchRequest& req,
    bool include_multiples
) {
    if (period < req.period_min) return;
    if (period <= req.period_max) push_unique_period(candidates, period);

    if (include_multiples && period >= 16 && period < req.period_max) {
        int added = 0;
        for (int multiple = period * 2; multiple <= req.period_max; multiple += period) {
            push_unique_period(candidates, multiple);
            if (++added >= 12) break;
        }
    }

    for (int d = std::min(period / 2, req.period_max); d >= req.period_min; --d) {
        if (period % d == 0) push_unique_period(candidates, d);
    }
}

std::vector<double> center_period_probe_epsilons(const SpecialPointSearchRequest& req) {
    std::vector<double> epsilons;
    const double base = std::max(req.classify_eps, std::numeric_limits<double>::epsilon());
    epsilons.push_back(base);

    // The viewport center can be a few visible pixels away from the actual
    // nucleus. Use a looser probe only for task ordering; root acceptance still
    // goes through the strict classifier later.
    const double viewport_eps = req.viewport.enabled
        ? std::min(1e-8, std::max(base, req.viewport.scale * 2.0))
        : base;
    const double targets[] = {
        base * 10.0,
        viewport_eps,
    };
    for (double eps : targets) {
        if (!std::isfinite(eps) || eps <= base) continue;
        eps = std::min(1e-8, eps);
        bool duplicate = false;
        for (double existing : epsilons) {
            if (std::abs(existing - eps) <= existing * 0.01) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) epsilons.push_back(eps);
    }
    std::sort(epsilons.begin(), epsilons.end());
    return epsilons;
}

SpecialPointResult make_base_result(SpecialPointKind kind, int preperiod, int period, Z c) {
    c = normalize_root(c, 1e-13);
    SpecialPointResult out;
    out.kind = kind;
    out.preperiod = preperiod;
    out.period = period;
    out.re = c.real();
    out.im = c.imag();
    out.id = format_id(kind, preperiod, period, c);
    return out;
}

SpecialPointResult polish_result(const SpecialPointResult& input, const SpecialPointEnumRequest& req) {
    SpecialPointResult out = input;
    if (!input.accepted) return out;
    const Z c0(input.re, input.im);
    if (input.kind == SpecialPointKind::HyperbolicCenter) {
        out = newton_solve_center(c0, input.period, req);
    } else {
        out = newton_solve_misiurewicz(c0, input.preperiod, input.period, req);
    }
    return out;
}

void add_or_replace_root(std::vector<SpecialPointResult>& roots, const SpecialPointResult& root, double eps) {
    for (auto& existing : roots) {
        if (!same_root(existing, root, eps)) continue;
        if (root.residual < existing.residual) existing = root;
        return;
    }
    roots.push_back(root);
}

bool make_local_fallback_candidate(
    SpecialPointResult root,
    const SpecialPointSearchRequest& req,
    SpecialPointResult& fallback
) {
    if (!root.converged || !root.actual.found_repeat || root.actual.period < 1) return false;
    if (root.actual.is_center) {
        root.kind = SpecialPointKind::HyperbolicCenter;
        root.preperiod = 0;
        root.period = root.actual.period;
    } else if (root.actual.is_misiurewicz) {
        root.kind = SpecialPointKind::Misiurewicz;
        root.preperiod = root.actual.preperiod;
        root.period = root.actual.period;
    } else {
        return false;
    }

    root.accepted = false;
    root.fallback = true;
    root.reason = "fallback_highest_actual_period";
    root.visible = point_in_viewport(req.viewport, Z(root.re, root.im));
    root.id = format_id(root.kind, root.preperiod, root.period, Z(root.re, root.im));
    if (req.include_variant_compatibility) {
        root.variants = classify_variant_existence(
            Z(root.re, root.im), root.kind, root.preperiod, root.period, req.classify_eps);
    }
    fallback = std::move(root);
    return true;
}

bool better_local_fallback_candidate(
    const SpecialPointResult& candidate,
    const SpecialPointResult& current,
    bool has_current
) {
    if (!candidate.fallback || !candidate.actual.found_repeat) return false;
    if (!has_current) return true;
    if (candidate.actual.period != current.actual.period) return candidate.actual.period > current.actual.period;
    if (candidate.actual.preperiod != current.actual.preperiod) return candidate.actual.preperiod > current.actual.preperiod;
    return candidate.residual < current.residual;
}

SeedSolveOutcome solve_enum_seed(Z seed, const Task& task, const SpecialPointEnumRequest& req) {
    SeedSolveOutcome out;
    if (std::norm(seed) > 4.0) return out;
    out.seed_count = 1;

    SpecialPointResult root = task.kind == SpecialPointKind::HyperbolicCenter
        ? newton_solve_center(seed, task.period, req)
        : newton_solve_misiurewicz(seed, task.preperiod, task.period, req);

    if (root.converged) ++out.newton_success_count;
    if (!root.accepted) {
        ++out.rejected_count;
        if (req.include_rejected_debug) out.rejected_debug.push_back(root);
        return out;
    }

    root = polish_result(root, req);
    if (!root.accepted) {
        ++out.rejected_count;
        if (req.include_rejected_debug) out.rejected_debug.push_back(root);
        return out;
    }

    root.visible = point_in_viewport(req.viewport, Z(root.re, root.im));
    if (!req.visible_only || root.visible) out.accepted.push_back(root);

    if (std::abs(root.im) > req.root_merge_eps) {
        const Z conj_seed(root.re, -root.im);
        SpecialPointResult conj_root = task.kind == SpecialPointKind::HyperbolicCenter
            ? newton_solve_center(conj_seed, task.period, req)
            : newton_solve_misiurewicz(conj_seed, task.preperiod, task.period, req);
        if (conj_root.accepted) {
            conj_root = polish_result(conj_root, req);
            conj_root.visible = point_in_viewport(req.viewport, Z(conj_root.re, conj_root.im));
            if (!req.visible_only || conj_root.visible) out.accepted.push_back(conj_root);
        }
    }
    return out;
}

void merge_enum_outcome(SpecialPointEnumResponse& resp, const SeedSolveOutcome& outcome, const SpecialPointEnumRequest& req) {
    resp.seed_count += outcome.seed_count;
    resp.newton_success_count += outcome.newton_success_count;
    resp.rejected_count += outcome.rejected_count;
    for (const auto& root : outcome.accepted) {
        add_or_replace_root(resp.points, root, req.root_merge_eps);
    }
    if (req.include_rejected_debug) {
        for (const auto& rejected : outcome.rejected_debug) {
            if (resp.rejected_debug.size() >= 256) break;
            resp.rejected_debug.push_back(rejected);
        }
    }
}

SeedSolveOutcome solve_search_seed(
    Z seed,
    const Task& task,
    const SpecialPointEnumRequest& opt,
    const SpecialPointSearchRequest& req
) {
    SeedSolveOutcome out;
    if (std::norm(seed) > 4.0) return out;
    out.seed_count = 1;

    SpecialPointResult root;
    int iter_limit = std::max(1, opt.max_newton_iter);
    for (;;) {
        SpecialPointEnumRequest attempt_opt = opt;
        attempt_opt.max_newton_iter = iter_limit;
        root = task.kind == SpecialPointKind::HyperbolicCenter
            ? newton_solve_center(seed, task.period, attempt_opt)
            : newton_solve_misiurewicz(seed, task.preperiod, task.period, attempt_opt);
        if (root.converged || root.reason != "not_converged" || iter_limit >= LOCAL_ADAPTIVE_NEWTON_MAX_ITER) break;
        iter_limit = std::min(LOCAL_ADAPTIVE_NEWTON_MAX_ITER, std::max(iter_limit + 1, iter_limit * 2));
    }
    if (root.converged) ++out.newton_success_count;
    if (!root.accepted) {
        ++out.rejected_count;
        SpecialPointResult fallback;
        if (make_local_fallback_candidate(root, req, fallback) && (!req.visible_only || fallback.visible)) {
            out.has_fallback_candidate = true;
            out.fallback_candidate = std::move(fallback);
        }
        return out;
    }

    root.visible = point_in_viewport(req.viewport, Z(root.re, root.im));
    if (!req.visible_only || root.visible) out.accepted.push_back(root);
    return out;
}

void merge_search_outcome(SpecialPointSearchResponse& resp, const SeedSolveOutcome& outcome, double merge_eps) {
    resp.seed_count += outcome.seed_count;
    resp.newton_success_count += outcome.newton_success_count;
    resp.rejected_count += outcome.rejected_count;
    for (const auto& root : outcome.accepted) {
        add_or_replace_root(resp.points, root, merge_eps);
    }
}

std::vector<Task> build_tasks(const SpecialPointEnumRequest& req) {
    std::vector<Task> tasks;
    if (req.kind == SpecialPointKind::HyperbolicCenter) {
        for (int p = req.period_min; p <= req.period_max; ++p) {
            tasks.push_back({req.kind, 0, p, expected_center_count(p)});
        }
    } else {
        for (int m = req.preperiod_min; m <= req.preperiod_max; ++m) {
            for (int p = req.misiurewicz_period_min; p <= req.misiurewicz_period_max; ++p) {
                tasks.push_back({req.kind, m, p, expected_misiurewicz_count(m, p)});
            }
        }
    }
    return tasks;
}

std::vector<int> estimated_center_period_candidates(
    Z initial,
    const SpecialPointSearchRequest& req
) {
    std::vector<int> candidates;
    if (req.kind != SpecialPointKind::HyperbolicCenter ||
        req.period_min > req.period_max ||
        !finite(initial)) {
        return candidates;
    }

    const int max_iter = std::max(
        64,
        std::min(200000, std::max(req.seed_budget, req.period_max * 8 + 32)));
    const std::vector<double> probe_epsilons = center_period_probe_epsilons(req);
    for (double eps : probe_epsilons) {
        const OrbitClassification orbit = classify_critical_orbit_mandelbrot(initial, max_iter, eps);
        if (!orbit.found_repeat || orbit.period < req.period_min) continue;

        const bool relaxed_probe = eps > req.classify_eps * 1.5;
        append_period_family(candidates, orbit.period, req, relaxed_probe);
        if (!candidates.empty()) break;
    }
    return candidates;
}

bool contains_period_candidate(const std::vector<int>& periods, int period) {
    return std::find(periods.begin(), periods.end(), period) != periods.end();
}

SpecialPointEnumRequest options_from_search(const SpecialPointSearchRequest& req) {
    SpecialPointEnumRequest out;
    out.kind = req.kind;
    out.period_min = req.period_min;
    out.period_max = req.period_max;
    out.preperiod_min = req.preperiod_min;
    out.preperiod_max = req.preperiod_max;
    out.misiurewicz_period_min = req.period_min;
    out.misiurewicz_period_max = req.period_max;
    out.max_newton_iter = req.max_newton_iter;
    out.newton_eps = req.newton_eps;
    out.classify_eps = req.classify_eps;
    out.root_merge_eps = req.root_merge_eps;
    out.include_variant_existence = req.include_variant_compatibility;
    out.visible_only = req.visible_only;
    out.viewport = req.viewport;
    return out;
}

} // namespace

std::string special_point_kind_name(SpecialPointKind kind) {
    return kind == SpecialPointKind::HyperbolicCenter ? "center" : "misiurewicz";
}

std::pair<Z, Z> eval_center_f_df(Z c, int period) {
    Z z{0.0, 0.0};
    Z dz{0.0, 0.0};
    for (int i = 0; i < period; ++i) {
        dz = 2.0 * z * dz + Z{1.0, 0.0};
        z = z * z + c;
    }
    return {z, dz};
}

std::pair<Z, Z> eval_misiurewicz_f_df(Z c, int preperiod, int period) {
    Z z{0.0, 0.0};
    Z dz{0.0, 0.0};
    Z z_m{0.0, 0.0};
    Z dz_m{0.0, 0.0};
    const int total = preperiod + period;
    for (int i = 0; i < total; ++i) {
        dz = 2.0 * z * dz + Z{1.0, 0.0};
        z = z * z + c;
        if (i + 1 == preperiod) {
            z_m = z;
            dz_m = dz;
        }
    }
    return {z - z_m, dz - dz_m};
}

OrbitClassification classify_critical_orbit_mandelbrot(Z c, int max_iter, double eps) {
    return classify_orbit_with_step(c, max_iter, eps, [](Z z, Z cc) {
        return z * z + cc;
    });
}

OrbitClassification classify_critical_orbit(Z c, int max_iter, double eps) {
    return classify_critical_orbit_mandelbrot(c, max_iter, eps);
}

std::vector<VariantExistence> classify_variant_existence(
    Z c,
    SpecialPointKind requested_kind,
    int requested_preperiod,
    int requested_period,
    double eps
) {
    static constexpr Variant variants[] = {
        Variant::Mandelbrot, Variant::Tri,  Variant::Boat, Variant::Duck, Variant::Bell,
        Variant::Fish,       Variant::Vase, Variant::Bird, Variant::Mask, Variant::Ship,
    };

    const OrbitClassification mandel = classify_critical_orbit_mandelbrot(
        c, std::max(8, requested_preperiod + requested_period + 8), eps);

    std::vector<VariantExistence> out;
    out.reserve(std::size(variants));
    for (Variant v : variants) {
        VariantExistence item;
        item.variant_name = display_variant_name(v);

        double max_step_error = 0.0;
        item.same_orbit_as_mandelbrot = true;
        if (mandel.orbit.size() >= 2) {
            for (size_t i = 0; i + 1 < mandel.orbit.size(); ++i) {
                const Z variant_next = variant_step_switch(v, mandel.orbit[i], c);
                const double err = std::abs(variant_next - mandel.orbit[i + 1]);
                max_step_error = std::max(max_step_error, err);
                if (err >= eps) item.same_orbit_as_mandelbrot = false;
            }
        }

        const OrbitClassification actual = classify_orbit_with_step(
            c, std::max(8, requested_preperiod + requested_period + 8), eps,
            [v](Z z, Z cc) { return variant_step_switch(v, z, cc); });

        item.exists = matches_request(actual, requested_kind, requested_preperiod, requested_period);
        item.actual_preperiod = actual.preperiod;
        item.actual_period = actual.period;
        item.repeat_error = actual.repeat_error;
        item.reason = item.exists ? "ok" : "not_same_orbit_or_not_matching_period";
        if (!item.same_orbit_as_mandelbrot) {
            item.reason = "not_same_orbit_or_not_matching_period";
        }
        item.repeat_error = std::max(item.repeat_error, max_step_error);
        out.push_back(item);
    }
    return out;
}

SpecialPointResult newton_solve_center(Z initial, int period, const SpecialPointEnumRequest& req) {
    SpecialPointResult out = make_base_result(SpecialPointKind::HyperbolicCenter, 0, period, initial);
    Z c = initial;
    const double accept_eps = newton_accept_eps(req);
    for (int iter = 0; iter < req.max_newton_iter; ++iter) {
        if (!finite(c)) {
            out.reason = "non_finite";
            return out;
        }
        if (std::abs(c) > 4.0) {
            out.reason = "escaped_parameter_region";
            return out;
        }
        const auto [f, df] = eval_center_f_df(c, period);
        out.residual = std::abs(f);
        out.newton_iterations = iter;
        if (out.residual < accept_eps) {
            out.converged = true;
            if (std::abs(df) < 1e-20) break;
            const Z step = f / df;
            if (std::abs(step) < 1e-14) break;
            c -= step;
            continue;
        }
        if (std::abs(df) < 1e-20) {
            out.reason = "derivative_too_small";
            return out;
        }
        const Z step = f / df;
        c -= step;
        if (std::abs(step) < 1e-16 && out.residual >= accept_eps) {
            out.reason = "stalled";
            return out;
        }
    }

    const auto [f, df] = eval_center_f_df(c, period);
    (void)df;
    const int iterations_used = out.newton_iterations;
    out = make_base_result(SpecialPointKind::HyperbolicCenter, 0, period, c);
    out.newton_iterations = iterations_used;
    out.residual = std::abs(f);
    out.converged = out.residual < accept_eps;
    if (!out.converged) {
        out.reason = "not_converged";
        return out;
    }
    out.actual = classify_critical_orbit_mandelbrot(c, std::max(16, period * 3 + 8), req.classify_eps);
    out.accepted = out.converged && matches_request(out.actual, out.kind, 0, period);
    out.reason = out.accepted ? "ok" : rejection_reason(out.actual, out.kind, 0, period);
    if (req.include_variant_existence && out.accepted) {
        out.variants = classify_variant_existence(c, out.kind, 0, period, req.classify_eps);
    }
    return out;
}

SpecialPointResult find_hyperbolic_center_near(Z initial, int period, const SpecialPointSearchRequest& req) {
    SpecialPointEnumRequest opt = options_from_search(req);
    return newton_solve_center(initial, period, opt);
}

SpecialPointResult newton_solve_misiurewicz(Z initial, int preperiod, int period, const SpecialPointEnumRequest& req) {
    SpecialPointResult out = make_base_result(SpecialPointKind::Misiurewicz, preperiod, period, initial);
    Z c = initial;
    const double accept_eps = newton_accept_eps(req);
    for (int iter = 0; iter < req.max_newton_iter; ++iter) {
        if (!finite(c)) {
            out.reason = "non_finite";
            return out;
        }
        if (std::abs(c) > 4.0) {
            out.reason = "escaped_parameter_region";
            return out;
        }
        const auto [f, df] = eval_misiurewicz_f_df(c, preperiod, period);
        out.residual = std::abs(f);
        out.newton_iterations = iter;
        if (out.residual < accept_eps) {
            out.converged = true;
            if (std::abs(df) < 1e-20) break;
            const Z step = f / df;
            if (std::abs(step) < 1e-14) break;
            c -= step;
            continue;
        }
        if (std::abs(df) < 1e-20) {
            out.reason = "derivative_too_small";
            return out;
        }
        const Z step = f / df;
        c -= step;
        if (std::abs(step) < 1e-16 && out.residual >= accept_eps) {
            out.reason = "stalled";
            return out;
        }
    }

    const auto [f, df] = eval_misiurewicz_f_df(c, preperiod, period);
    (void)df;
    const int iterations_used = out.newton_iterations;
    out = make_base_result(SpecialPointKind::Misiurewicz, preperiod, period, c);
    out.newton_iterations = iterations_used;
    out.residual = std::abs(f);
    out.converged = out.residual < accept_eps;
    if (!out.converged) {
        out.reason = "not_converged";
        return out;
    }
    out.actual = classify_critical_orbit_mandelbrot(
        c, std::max(16, (preperiod + period) * 3 + 8), req.classify_eps);
    out.accepted = out.converged && matches_request(out.actual, out.kind, preperiod, period);
    out.reason = out.accepted ? "ok" : rejection_reason(out.actual, out.kind, preperiod, period);
    if (req.include_variant_existence && out.accepted) {
        out.variants = classify_variant_existence(c, out.kind, preperiod, period, req.classify_eps);
    }
    return out;
}

int expected_center_count(int period) {
    if (period < 1 || period > 30) return -1;
    return mobius_center_count(period);
}

int expected_misiurewicz_count(int preperiod, int period) {
    if (preperiod < 1 || period < 1 || preperiod > 6 || period > 6 || preperiod + period > 10) {
        return -1;
    }
    if (preperiod == 1) return 0;

    // Exact parameter count for the quadratic critical orbit definition used
    // here: z_0 = 0, exact preperiod m, exact period p. The primitive period-p
    // point count of z -> z^2 + c is 2 * expected_center_count(p). For m >= 2,
    // each primitive periodic point has 2^(m-2) critical preimage choices,
    // except when the critical orbit itself is a period-p center
    // (p | (m - 1)); those centers are not Misiurewicz points.
    const int primitive_periodic_points = 2 * expected_center_count(period);
    int count = primitive_periodic_points * (1 << (preperiod - 2));
    if ((preperiod - 1) % period == 0) count -= expected_center_count(period);
    return count;
}

bool point_in_viewport(const SpecialPointViewport& viewport, Z c) {
    if (!viewport.enabled) return true;
    const double aspect = static_cast<double>(std::max(1, viewport.width)) / static_cast<double>(std::max(1, viewport.height));
    const double half_h = viewport.scale * 0.5;
    const double half_w = half_h * aspect;
    return c.real() >= viewport.center_re - half_w
        && c.real() <= viewport.center_re + half_w
        && c.imag() >= viewport.center_im - half_h
        && c.imag() <= viewport.center_im + half_h;
}

SpecialPointEnumResponse enumerate_special_points(
    const SpecialPointEnumRequest& req,
    const SpecialPointProgressCallback& progress
) {
    const std::vector<Task> tasks = build_tasks(req);
    SpecialPointEnumResponse resp;
    resp.status = "running";
    for (const Task& t : tasks) {
        if (t.expected < 0) {
            throw std::runtime_error("expected count unavailable for requested Misiurewicz parameter");
        }
        resp.expected_count += t.expected;
    }

    int task_index = 0;
    for (const Task& task : tasks) {
        ++task_index;
        const int accepted_before_task = static_cast<int>(resp.points.size());
        const unsigned long long task_seed_offset =
            1000003ULL * static_cast<unsigned long long>(task.period)
            + 9176ULL * static_cast<unsigned long long>(task.preperiod + 1);

        const Z anchors[] = {
            {0.0, 0.0}, {-1.0, 0.0}, {-2.0, 0.0}, {1.0, 0.0}, {0.25, 0.0},
            {0.0, 1.0}, {0.0, -1.0}, {-0.75, 0.75}, {-0.75, -0.75},
        };
        for (Z seed : anchors) {
            if (static_cast<int>(resp.points.size()) - accepted_before_task >= task.expected) break;
            merge_enum_outcome(resp, solve_enum_seed(seed, task, req), req);
        }

        for (int batch = 0; batch < req.max_seed_batches; ++batch) {
            if (static_cast<int>(resp.points.size()) - accepted_before_task >= task.expected) break;
            if (progress && !progress(task_index, static_cast<int>(tasks.size()),
                                      static_cast<int>(resp.points.size()), resp.expected_count,
                                      resp.seed_count, batch)) {
                resp.status = "cancelled";
                resp.complete = false;
                return resp;
            }

            std::vector<Z> seeds;
            seeds.reserve(static_cast<size_t>(req.seeds_per_batch));
            for (int i = 0; i < req.seeds_per_batch; ++i) {
                const unsigned long long seed_index = task_seed_offset
                    + static_cast<unsigned long long>(batch) * static_cast<unsigned long long>(req.seeds_per_batch)
                    + static_cast<unsigned long long>(i);
                seeds.push_back(seed_for(seed_index));
            }

            std::vector<SeedSolveOutcome> outcomes(seeds.size());
            #pragma omp parallel for schedule(dynamic, 16)
            for (int i = 0; i < static_cast<int>(seeds.size()); ++i) {
                outcomes[static_cast<size_t>(i)] = solve_enum_seed(seeds[static_cast<size_t>(i)], task, req);
            }

            for (const auto& outcome : outcomes) {
                merge_enum_outcome(resp, outcome, req);
            }
        }
    }

    std::sort(resp.points.begin(), resp.points.end(), [](const auto& a, const auto& b) {
        if (a.period != b.period) return a.period < b.period;
        if (a.preperiod != b.preperiod) return a.preperiod < b.preperiod;
        if (a.re != b.re) return a.re < b.re;
        return a.im < b.im;
    });

    resp.accepted_count = static_cast<int>(resp.points.size());
    resp.complete = !req.visible_only && resp.accepted_count == resp.expected_count;
    resp.status = resp.complete ? "completed" : "incomplete";
    if (!resp.complete) {
        resp.warning = "not all expected roots were found within the seed budget";
        if (req.visible_only) resp.warning = "visibleOnly filters results; complete is only true for unfiltered enumeration";
    }
    return resp;
}

SpecialPointSearchResponse search_special_points(
    const SpecialPointSearchRequest& req,
    const SpecialPointSearchProgressCallback& progress
) {
    if (!req.viewport.enabled) {
        throw std::runtime_error("viewport is required for special point search");
    }

    // Deep zoom: double precision cannot separate roots below the threshold,
    // hand off to the MPFR ball-period + Newton solver.
    if (special_points_search_wants_hp(req)) {
        return search_special_points_hp(req, progress);
    }

    SpecialPointSearchResponse resp;
    resp.status = "searching";
    resp.sampled = false;

    std::vector<Task> tasks;
    if (req.kind == SpecialPointKind::HyperbolicCenter) {
        for (int p = req.period_min; p <= req.period_max; ++p) {
            tasks.push_back({req.kind, 0, p, 0});
        }
    } else {
        for (int m = req.preperiod_min; m <= req.preperiod_max; ++m) {
            for (int p = req.period_min; p <= req.period_max; ++p) {
                tasks.push_back({req.kind, m, p, 0});
            }
        }
    }
    const int task_count = std::max(1, static_cast<int>(tasks.size()));
    const int progress_total_hint = req.kind == SpecialPointKind::HyperbolicCenter
        ? task_count + std::max(0, viewport_search_seed_limit(req) - 1)
        : task_count;
    const double merge_eps = std::max(req.root_merge_eps, req.viewport.scale * 1e-9);
    const SpecialPointEnumRequest opt = options_from_search(req);
    const Z initial{req.viewport.center_re, req.viewport.center_im};
    SpecialPointResult best_fallback;
    bool has_fallback = false;
    int progress_index = 0;

    auto report_progress = [&](const Task& task) -> bool {
        if (progress) {
            const int period_index = ++progress_index;
            const bool proceed = progress(
                task.period, period_index, std::max(progress_total_hint, period_index),
                static_cast<int>(resp.points.size()), resp.seed_count);
            if (!proceed) {
                resp.status = "cancelled";
                return false;
            }
        }
        return true;
    };

    auto merge_outcome = [&](const SeedSolveOutcome& outcome) -> bool {
        merge_search_outcome(resp, outcome, merge_eps);
        if (outcome.has_fallback_candidate &&
            better_local_fallback_candidate(outcome.fallback_candidate, best_fallback, has_fallback)) {
            best_fallback = outcome.fallback_candidate;
            has_fallback = true;
        }
        return !outcome.accepted.empty();
    };

    auto run_task = [&](Z seed, const Task& task) -> SearchStepResult {
        if (!report_progress(task)) return {false, true};
        return {merge_outcome(solve_search_seed(seed, task, opt, req)), false};
    };

    auto run_tasks_for_seed = [&](Z seed, const std::vector<Task>& selected) -> SearchStepResult {
        for (const Task& task : selected) {
            const SearchStepResult step = run_task(seed, task);
            if (step.accepted || step.cancelled) return step;
        }
        return {};
    };

    auto solve_seed_batch = [&](const std::vector<Z>& seeds, const std::vector<size_t>& seed_indices, const Task& task) -> SearchStepResult {
        if (seed_indices.empty()) return {};
        if (!report_progress(task)) return {false, true};

        std::vector<SeedSolveOutcome> outcomes(seed_indices.size());
        bool used_cuda_batch = false;

#if USE_CUDA_SPECIAL_POINTS
        if (task.kind == SpecialPointKind::HyperbolicCenter &&
            special_point_cuda_enabled() &&
            static_cast<int>(seed_indices.size()) >= special_point_cuda_seed_batch_min() &&
            fsd_cuda::cuda_special_points_available()) {
            try {
                std::vector<fsd_cuda::CudaCenterSeed> cuda_seeds;
                cuda_seeds.reserve(seed_indices.size());
                for (size_t seed_index : seed_indices) {
                    const Z seed = seeds[seed_index];
                    cuda_seeds.push_back({seed.real(), seed.imag()});
                }

                const std::vector<fsd_cuda::CudaCenterNewtonResult> cuda_results =
                    fsd_cuda::cuda_solve_center_batch(
                        cuda_seeds,
                        task.period,
                        std::max(opt.max_newton_iter, LOCAL_ADAPTIVE_NEWTON_MAX_ITER),
                        newton_accept_eps(opt));

                for (size_t i = 0; i < cuda_results.size(); ++i) {
                    const auto& cuda_root = cuda_results[i];
                    SeedSolveOutcome out;
                    out.seed_count = 1;
                    if (cuda_root.converged) {
                        SpecialPointEnumRequest polish_opt = opt;
                        polish_opt.max_newton_iter = std::max(opt.max_newton_iter, LOCAL_ADAPTIVE_NEWTON_MAX_ITER);
                        SpecialPointResult root = newton_solve_center(
                            {cuda_root.re, cuda_root.im},
                            task.period,
                            polish_opt);
                        if (!root.accepted && !root.converged) {
                            root = make_base_result(
                                SpecialPointKind::HyperbolicCenter,
                                0,
                                task.period,
                                {cuda_root.re, cuda_root.im});
                            root.converged = true;
                            root.residual = cuda_root.residual;
                            root.newton_iterations = cuda_root.iterations;
                            root.actual = classify_critical_orbit_mandelbrot(
                                {cuda_root.re, cuda_root.im},
                                std::max(16, task.period * 3 + 8),
                                req.classify_eps);
                            root.accepted = matches_request(root.actual, root.kind, 0, task.period);
                            root.reason = root.accepted ? "ok" : rejection_reason(root.actual, root.kind, 0, task.period);
                            if (req.include_variant_compatibility && root.accepted) {
                                root.variants = classify_variant_existence(
                                    {cuda_root.re, cuda_root.im},
                                    root.kind,
                                    0,
                                    task.period,
                                    req.classify_eps);
                            }
                        }
                        ++out.newton_success_count;
                        if (root.accepted) {
                            root.visible = point_in_viewport(req.viewport, {root.re, root.im});
                            if (!req.visible_only || root.visible) out.accepted.push_back(std::move(root));
                        } else {
                            ++out.rejected_count;
                        }
                    } else {
                        ++out.rejected_count;
                    }
                    outcomes[i] = std::move(out);
                }
                used_cuda_batch = true;
            } catch (...) {
                used_cuda_batch = false;
            }
        }
#endif

        if (used_cuda_batch &&
            std::none_of(outcomes.begin(), outcomes.end(), [](const SeedSolveOutcome& outcome) {
                return !outcome.accepted.empty();
            })) {
            #pragma omp parallel for schedule(dynamic, 1)
            for (int i = 0; i < static_cast<int>(seed_indices.size()); ++i) {
                const size_t seed_index = seed_indices[static_cast<size_t>(i)];
                outcomes[static_cast<size_t>(i)] = solve_search_seed(seeds[seed_index], task, opt, req);
            }
        }

        if (!used_cuda_batch) {
            #pragma omp parallel for schedule(dynamic, 1)
            for (int i = 0; i < static_cast<int>(seed_indices.size()); ++i) {
                const size_t seed_index = seed_indices[static_cast<size_t>(i)];
                outcomes[static_cast<size_t>(i)] = solve_search_seed(seeds[seed_index], task, opt, req);
            }
        }

        bool accepted = false;
        for (const auto& outcome : outcomes) {
            if (merge_outcome(outcome)) accepted = true;
        }
        return {accepted, false};
    };

    bool done = false;
    if (req.kind == SpecialPointKind::HyperbolicCenter) {
        const std::vector<Z> seeds = viewport_search_seeds(req);
        std::vector<std::vector<int>> seed_period_hints(seeds.size());
        bool seed_period_hints_ready = false;

        auto ensure_seed_period_hints = [&]() {
            if (seed_period_hints_ready) return;
            #pragma omp parallel for schedule(dynamic, 1)
            for (int i = 1; i < static_cast<int>(seeds.size()); ++i) {
                seed_period_hints[static_cast<size_t>(i)] =
                    estimated_center_period_candidates(seeds[static_cast<size_t>(i)], req);
            }
            seed_period_hints_ready = true;
        };

        const int period_wave_size = local_period_wave_size();
        for (size_t block_start = 0; block_start < tasks.size() && !done; block_start += static_cast<size_t>(period_wave_size)) {
            const size_t block_end = std::min(tasks.size(), block_start + static_cast<size_t>(period_wave_size));

            std::vector<SeedSolveOutcome> center_outcomes(block_end - block_start);
            #pragma omp parallel for schedule(dynamic, 1)
            for (int i = 0; i < static_cast<int>(center_outcomes.size()); ++i) {
                const Task& task = tasks[block_start + static_cast<size_t>(i)];
                center_outcomes[static_cast<size_t>(i)] = solve_search_seed(initial, task, opt, req);
            }

            bool center_wave_accepted = false;
            for (size_t task_i = block_start; task_i < block_end; ++task_i) {
                const Task& task = tasks[task_i];
                if (!report_progress(task)) return resp;
                if (merge_outcome(center_outcomes[task_i - block_start])) {
                    center_wave_accepted = true;
                }
            }
            if (center_wave_accepted) {
                done = true;
                break;
            }

            for (size_t task_i = block_start; task_i < block_end; ++task_i) {
                const Task& task = tasks[task_i];
                ensure_seed_period_hints();
                std::vector<size_t> seed_indices;
                seed_indices.reserve(seeds.size());
                for (size_t seed_i = 1; seed_i < seeds.size(); ++seed_i) {
                    if (contains_period_candidate(seed_period_hints[seed_i], task.period)) {
                        seed_indices.push_back(seed_i);
                    }
                }
                if (seed_indices.empty()) continue;

                resp.sampled = true;
                const SearchStepResult step = solve_seed_batch(seeds, seed_indices, task);
                if (step.cancelled) return resp;
                if (step.accepted) {
                    done = true;
                    break;
                }
            }
        }
    } else {
        const SearchStepResult step = run_tasks_for_seed(initial, tasks);
        if (step.cancelled) return resp;
    }

    std::sort(resp.points.begin(), resp.points.end(), [](const auto& a, const auto& b) {
        if (a.period != b.period) return a.period < b.period;
        if (a.preperiod != b.preperiod) return a.preperiod < b.preperiod;
        if (a.re != b.re) return a.re < b.re;
        return a.im < b.im;
    });
    if (resp.points.empty() && has_fallback) {
        resp.points.push_back(best_fallback);
    }

    resp.accepted_count = static_cast<int>(std::count_if(resp.points.begin(), resp.points.end(), [](const auto& p) {
        return p.accepted;
    }));
    resp.fallback_count = static_cast<int>(std::count_if(resp.points.begin(), resp.points.end(), [](const auto& p) {
        return p.fallback;
    }));
    resp.status = "completed";
    if (resp.fallback_count > 0) {
        resp.warning = req.kind == SpecialPointKind::Misiurewicz
            ? "no exact local Misiurewicz match found; showing the highest-period classified fallback candidate"
            : "no exact viewport hyperbolic center match found; showing the highest-period classified fallback candidate";
    } else if (resp.accepted_count == 0) {
        resp.warning = req.kind == SpecialPointKind::Misiurewicz
            ? "no matching local Misiurewicz point found from the current center"
            : (resp.sampled
                ? "no matching local hyperbolic center found in the current viewport samples"
                : "no matching local hyperbolic center found from the current center");
    } else {
        resp.warning = req.kind == SpecialPointKind::Misiurewicz
            ? "local Misiurewicz solve from current center; stops after the first matching point"
            : (resp.sampled
                ? "viewport hyperbolic center solve; stopped after the first matching wave and marked all computed matches"
                : "local hyperbolic center solve; stopped after the first matching wave and marked all computed matches");
    }
    return resp;
}

} // namespace fsd::compute
