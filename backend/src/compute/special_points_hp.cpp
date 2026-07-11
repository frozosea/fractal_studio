// special_points_hp.cpp — deep-zoom special point solving in MPFR.
//
// Below special_points_hp_scale_threshold() the double solver cannot separate
// candidate roots from the viewport center (the center itself arrives rounded
// to ~1e-16), so search_special_points() delegates here. The flow is the
// standard deep-zoom nucleus finder:
//
//   1. Ball-arithmetic period detection: iterate the critical orbit of the
//      viewport center at full precision while growing a radius that bounds
//      every orbit started inside the viewport disk. Iterations where the
//      ball contains 0 are candidate periods of minibrots overlapping the
//      view.
//   2. Newton refinement of the nucleus equation z_p(c) = 0 (or
//      z_{m+p}(c) = z_m(c) for Misiurewicz points) at full precision.
//   3. Divisor-based classification: the actual period is the smallest
//      divisor q of p with |z_q| below a depth-aware threshold; Misiurewicz
//      roots additionally get minimal-preperiod and center-degeneracy checks.
//
// Coordinates cross the API as decimal strings (viewport.center_re_str /
// result.re_str), the same convention the perturbation renderer uses for its
// reference orbits.

#include "special_points.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(FSD_HAS_MPFR)
#  include <mpfr.h>
#endif

namespace fsd::compute {
namespace {

constexpr double HP_DEFAULT_SCALE_THRESHOLD = 1e-8;
constexpr int HP_MAX_BALL_CANDIDATES = 12;
constexpr int HP_NEWTON_MAX_ITER = 64;
constexpr long long HP_CENTER_STEP_BUDGET = 48'000'000;
constexpr long long HP_MISIUREWICZ_STEP_BUDGET = 24'000'000;

bool hp_enabled_env() {
    const char* raw = std::getenv("FSD_SPECIAL_POINT_HP");
    if (!raw || *raw == '\0') return true;
    return !(raw[0] == '0' || raw[0] == 'f' || raw[0] == 'F' ||
             raw[0] == 'n' || raw[0] == 'N');
}

double viewport_aspect(const SpecialPointViewport& v) {
    return static_cast<double>(std::max(1, v.width)) /
        static_cast<double>(std::max(1, v.height));
}

} // namespace

double special_points_hp_scale_threshold() {
    const char* raw = std::getenv("FSD_SPECIAL_POINT_HP_SCALE");
    if (raw && *raw != '\0') {
        char* end = nullptr;
        const double parsed = std::strtod(raw, &end);
        if (end != raw && *end == '\0' && std::isfinite(parsed) && parsed > 0.0 && parsed <= 1.0) {
            return parsed;
        }
    }
    return HP_DEFAULT_SCALE_THRESHOLD;
}

bool special_points_search_wants_hp(const SpecialPointSearchRequest& req) {
    if (!special_points_hp_available() || !hp_enabled_env()) return false;
    if (!req.viewport.enabled) return false;
    const double scale = req.viewport.scale;
    return std::isfinite(scale) && scale > 0.0 && scale < special_points_hp_scale_threshold();
}

#if !defined(FSD_HAS_MPFR)

bool special_points_hp_available() { return false; }

SpecialPointSearchResponse search_special_points_hp(
    const SpecialPointSearchRequest&,
    const SpecialPointSearchProgressCallback&
) {
    SpecialPointSearchResponse resp;
    resp.status = "completed";
    resp.warning = "high-precision special point solver unavailable (built without MPFR)";
    return resp;
}

#else

namespace {

// ---------------------------------------------------------------------------
// Minimal MPFR complex helpers. Perturbation reference orbits already use raw
// mpfr_t pairs; the same style is kept here, wrapped for exception safety.
// ---------------------------------------------------------------------------

struct HpC {
    mpfr_t re, im;
    explicit HpC(mpfr_prec_t prec) {
        mpfr_init2(re, prec);
        mpfr_init2(im, prec);
        mpfr_set_zero(re, 1);
        mpfr_set_zero(im, 1);
    }
    ~HpC() {
        mpfr_clear(re);
        mpfr_clear(im);
    }
    HpC(const HpC&) = delete;
    HpC& operator=(const HpC&) = delete;
};

struct HpWork {
    mpfr_t t0, t1, t2, t3;
    explicit HpWork(mpfr_prec_t prec) {
        mpfr_inits2(prec, t0, t1, t2, t3, static_cast<mpfr_ptr>(nullptr));
    }
    ~HpWork() {
        mpfr_clears(t0, t1, t2, t3, static_cast<mpfr_ptr>(nullptr));
    }
    HpWork(const HpWork&) = delete;
    HpWork& operator=(const HpWork&) = delete;
};

struct HpReal {
    mpfr_t v;
    explicit HpReal(mpfr_prec_t prec) {
        mpfr_init2(v, prec);
        mpfr_set_zero(v, 1);
    }
    ~HpReal() { mpfr_clear(v); }
    HpReal(const HpReal&) = delete;
    HpReal& operator=(const HpReal&) = delete;
};

void hp_set(HpC& out, const HpC& a) {
    mpfr_set(out.re, a.re, MPFR_RNDN);
    mpfr_set(out.im, a.im, MPFR_RNDN);
}

// z <- z² + c (in-place safe)
void hp_step(HpC& z, const HpC& c, HpWork& w) {
    mpfr_sqr(w.t0, z.re, MPFR_RNDN);
    mpfr_sqr(w.t1, z.im, MPFR_RNDN);
    mpfr_mul(w.t2, z.re, z.im, MPFR_RNDN);
    mpfr_sub(w.t0, w.t0, w.t1, MPFR_RNDN);
    mpfr_add(z.re, w.t0, c.re, MPFR_RNDN);
    mpfr_mul_2ui(w.t2, w.t2, 1, MPFR_RNDN);
    mpfr_add(z.im, w.t2, c.im, MPFR_RNDN);
}

// d <- 2·z·d + 1, with z the value before the matching hp_step
void hp_step_derivative(HpC& d, const HpC& z, HpWork& w) {
    mpfr_mul(w.t0, z.re, d.re, MPFR_RNDN);
    mpfr_mul(w.t1, z.im, d.im, MPFR_RNDN);
    mpfr_sub(w.t0, w.t0, w.t1, MPFR_RNDN);
    mpfr_mul(w.t1, z.re, d.im, MPFR_RNDN);
    mpfr_mul(w.t2, z.im, d.re, MPFR_RNDN);
    mpfr_add(w.t1, w.t1, w.t2, MPFR_RNDN);
    mpfr_mul_2ui(w.t0, w.t0, 1, MPFR_RNDN);
    mpfr_mul_2ui(w.t1, w.t1, 1, MPFR_RNDN);
    mpfr_add_ui(d.re, w.t0, 1, MPFR_RNDN);
    mpfr_set(d.im, w.t1, MPFR_RNDN);
}

void hp_norm2(mpfr_t out, const HpC& a, HpWork& w) {
    mpfr_sqr(w.t0, a.re, MPFR_RNDN);
    mpfr_sqr(w.t1, a.im, MPFR_RNDN);
    mpfr_add(out, w.t0, w.t1, MPFR_RNDN);
}

void hp_sub(HpC& out, const HpC& a, const HpC& b) {
    mpfr_sub(out.re, a.re, b.re, MPFR_RNDN);
    mpfr_sub(out.im, a.im, b.im, MPFR_RNDN);
}

// out <- a / b; out must not alias a or b. False when |b|² == 0.
bool hp_div(HpC& out, const HpC& a, const HpC& b, HpWork& w, mpfr_t norm) {
    mpfr_sqr(w.t0, b.re, MPFR_RNDN);
    mpfr_sqr(w.t1, b.im, MPFR_RNDN);
    mpfr_add(norm, w.t0, w.t1, MPFR_RNDN);
    if (mpfr_zero_p(norm)) return false;
    mpfr_mul(w.t0, a.re, b.re, MPFR_RNDN);
    mpfr_mul(w.t1, a.im, b.im, MPFR_RNDN);
    mpfr_add(w.t0, w.t0, w.t1, MPFR_RNDN);
    mpfr_div(out.re, w.t0, norm, MPFR_RNDN);
    mpfr_mul(w.t0, a.im, b.re, MPFR_RNDN);
    mpfr_mul(w.t1, a.re, b.im, MPFR_RNDN);
    mpfr_sub(w.t0, w.t0, w.t1, MPFR_RNDN);
    mpfr_div(out.im, w.t0, norm, MPFR_RNDN);
    return true;
}

double hp_abs_d(const HpC& a, HpWork& w) {
    hp_norm2(w.t3, a, w);
    mpfr_sqrt(w.t3, w.t3, MPFR_RNDN);
    return mpfr_get_d(w.t3, MPFR_RNDN);
}

std::string hp_to_string(mpfr_srcptr v, int digits) {
    char* out = nullptr;
    if (mpfr_asprintf(&out, "%.*Rg", digits, v) < 0 || !out) return "0";
    std::string s(out);
    mpfr_free_str(out);
    return s;
}

// ---------------------------------------------------------------------------
// Depth-aware precision and tolerances.
//
// zoom_bits = -log2(scale). Newton is run to the precision floor, so the
// found nucleus carries a relative error of ~2^-bits and the residual
// |z_p(c)| lands around 2^(zoom_bits - bits). Near-returns of the orbit at
// non-period divisors sit near sqrt(minibrot size) ≈ 2^(-zoom_bits/2), so
// the classification threshold 2^(-zoom_bits/2 - 40) separates the two as
// long as bits grows 1.5× faster than the depth — hence the 3/2 slope.
// ---------------------------------------------------------------------------

int hp_zoom_bits(double scale) {
    if (!(scale > 0.0) || !std::isfinite(scale)) return 27;
    return std::max(0, static_cast<int>(std::ceil(-std::log2(scale))));
}

int hp_bits_for_scale(double scale) {
    const int zoom_bits = hp_zoom_bits(scale);
    return std::max(128, (zoom_bits * 3) / 2 + 96);
}

struct HpContext {
    mpfr_prec_t prec;
    int bits;
    int zoom_bits;
    double scale;
    double half_w;
    double half_h;
    int out_digits;   // digits for re_str/im_str
    int id_digits;    // digits used inside point ids
    HpC center;
    HpReal eps2;      // squared classification threshold
    HpReal tol2;      // squared Newton step floor
    HpReal esc2;      // squared local escape distance
    HpWork work;

    explicit HpContext(const SpecialPointViewport& v)
        : prec(hp_bits_for_scale(v.scale)),
          bits(hp_bits_for_scale(v.scale)),
          zoom_bits(hp_zoom_bits(v.scale)),
          scale(v.scale),
          half_w(0.0),
          half_h(0.0),
          out_digits(0),
          id_digits(0),
          center(prec),
          eps2(prec),
          tol2(prec),
          esc2(prec),
          work(prec) {
        half_h = v.scale * 0.5;
        half_w = half_h * viewport_aspect(v);
        out_digits = static_cast<int>(std::ceil(bits * 0.30103)) + 4;
        id_digits = std::min(out_digits, zoom_bits / 3 + 12);

        bool re_ok = false;
        bool im_ok = false;
        if (!v.center_re_str.empty()) {
            re_ok = mpfr_set_str(center.re, v.center_re_str.c_str(), 10, MPFR_RNDN) == 0;
        }
        if (!v.center_im_str.empty()) {
            im_ok = mpfr_set_str(center.im, v.center_im_str.c_str(), 10, MPFR_RNDN) == 0;
        }
        if (!re_ok) mpfr_set_d(center.re, v.center_re, MPFR_RNDN);
        if (!im_ok) mpfr_set_d(center.im, v.center_im, MPFR_RNDN);

        // eps² = 2^-(zoom_bits + 80)  → eps = 2^-(zoom_bits/2 + 40)
        mpfr_set_ui_2exp(eps2.v, 1, -(static_cast<mpfr_exp_t>(zoom_bits) + 80), MPFR_RNDN);
        // tol² = 2^-(2·bits - 16): iterate Newton down to the precision floor
        mpfr_set_ui_2exp(tol2.v, 1, -(2 * static_cast<mpfr_exp_t>(bits) - 16), MPFR_RNDN);
        // escape when the iterate leaves 64 viewport-diagonals
        const double diag = std::hypot(half_w, half_h);
        mpfr_set_d(esc2.v, diag, MPFR_RNDN);
        mpfr_mul_2ui(esc2.v, esc2.v, 6, MPFR_RNDN);
        mpfr_sqr(esc2.v, esc2.v, MPFR_RNDN);
    }
};

bool hp_in_viewport(const HpContext& ctx, const HpC& c, HpWork& w) {
    mpfr_sub(w.t0, c.re, ctx.center.re, MPFR_RNDN);
    mpfr_abs(w.t0, w.t0, MPFR_RNDN);
    if (mpfr_cmp_d(w.t0, ctx.half_w) > 0) return false;
    mpfr_sub(w.t0, c.im, ctx.center.im, MPFR_RNDN);
    mpfr_abs(w.t0, w.t0, MPFR_RNDN);
    return mpfr_cmp_d(w.t0, ctx.half_h) <= 0;
}

// ---------------------------------------------------------------------------
// Ball-arithmetic period detection. Iterate the critical orbit of the
// viewport center; the radius bounds |z_n(c') - z_n(c0)| over every c' within
// r0 of the center. When the ball contains 0 the iteration count is a
// candidate period of a minibrot overlapping the viewport disk.
// ---------------------------------------------------------------------------

std::vector<int> hp_ball_periods(
    const HpContext& ctx,
    int period_min,
    int period_max,
    int max_candidates
) {
    std::vector<int> candidates;
    HpC z(ctx.prec);
    HpWork w(ctx.prec);

    const double r0 = std::hypot(ctx.half_w, ctx.half_h);
    double r = r0;
    for (int n = 1; n <= period_max; ++n) {
        hp_step(z, ctx.center, w);
        const double abs_z = hp_abs_d(z, w);
        if (!std::isfinite(abs_z) || abs_z > 4.0) break;
        if (abs_z <= r) {
            if (n >= period_min) candidates.push_back(n);
            if (static_cast<int>(candidates.size()) >= max_candidates) break;
        }
        r = 2.0 * abs_z * r + r * r + r0;
        if (r > 2.0) break;  // ball covers the whole dynamic range
    }
    return candidates;
}

// ---------------------------------------------------------------------------
// Newton refinement.
// ---------------------------------------------------------------------------

struct HpNewtonOutcome {
    bool converged = false;
    std::string fail_reason = "not_converged";
    int iterations = 0;
    double residual = 0.0;
    long long steps = 0;  // orbit steps spent (budget accounting)
};

// f/df of the center equation z_period(c) = 0 at c.
void hp_center_f_df(const HpC& c, int period, HpC& f, HpC& df, HpWork& w) {
    HpC z(mpfr_get_prec(c.re));
    HpC dz(mpfr_get_prec(c.re));
    for (int i = 0; i < period; ++i) {
        hp_step_derivative(dz, z, w);
        hp_step(z, c, w);
    }
    hp_set(f, z);
    hp_set(df, dz);
}

// f/df of the Misiurewicz equation z_{m+p}(c) - z_m(c) = 0 at c.
void hp_misiurewicz_f_df(const HpC& c, int preperiod, int period, HpC& f, HpC& df, HpWork& w) {
    const mpfr_prec_t prec = mpfr_get_prec(c.re);
    HpC z(prec), dz(prec), zm(prec), dzm(prec);
    const int total = preperiod + period;
    for (int i = 0; i < total; ++i) {
        hp_step_derivative(dz, z, w);
        hp_step(z, c, w);
        if (i + 1 == preperiod) {
            hp_set(zm, z);
            hp_set(dzm, dz);
        }
    }
    hp_sub(f, z, zm);
    hp_sub(df, dz, dzm);
}

template <typename EvalFn>
HpNewtonOutcome hp_newton(
    const HpContext& ctx,
    HpC& c,               // in: seed, out: refined root
    int orbit_len,        // steps per f/df evaluation (budget accounting)
    int max_iter,
    const EvalFn& eval    // eval(c, f, df, w)
) {
    HpNewtonOutcome out;
    const mpfr_prec_t prec = ctx.prec;
    HpC f(prec), df(prec), step(prec), delta(prec);
    HpWork w(prec);
    HpReal norm(prec), n2(prec);

    for (int iter = 0; iter < max_iter; ++iter) {
        eval(c, f, df, w);
        out.steps += orbit_len;
        out.iterations = iter + 1;

        hp_norm2(n2.v, f, w);
        if (mpfr_zero_p(n2.v)) {
            out.converged = true;
            break;
        }
        if (!hp_div(step, f, df, w, norm.v)) {
            out.fail_reason = "derivative_too_small";
            return out;
        }
        mpfr_sub(c.re, c.re, step.re, MPFR_RNDN);
        mpfr_sub(c.im, c.im, step.im, MPFR_RNDN);

        hp_norm2(n2.v, c, w);
        if (!mpfr_number_p(n2.v) || mpfr_cmp_ui(n2.v, 16) > 0) {
            out.fail_reason = "escaped_parameter_region";
            return out;
        }
        hp_sub(delta, c, ctx.center);
        hp_norm2(n2.v, delta, w);
        if (mpfr_cmp(n2.v, ctx.esc2.v) > 0) {
            out.fail_reason = "escaped_local_region";
            return out;
        }

        hp_norm2(n2.v, step, w);
        if (mpfr_cmp(n2.v, ctx.tol2.v) <= 0) {
            out.converged = true;
            break;
        }
    }
    if (!out.converged) {
        out.fail_reason = "not_converged";
        return out;
    }

    // Residual at the accepted root, for reporting.
    eval(c, f, df, w);
    out.steps += orbit_len;
    hp_norm2(n2.v, f, w);
    mpfr_sqrt(n2.v, n2.v, MPFR_RNDN);
    out.residual = mpfr_get_d(n2.v, MPFR_RNDN);
    return out;
}

// ---------------------------------------------------------------------------
// Classification.
// ---------------------------------------------------------------------------

// Smallest divisor q of `period` with |z_q(c)| ≤ eps. Returns -1 when even
// z_period stays above the threshold (Newton landed off a nucleus).
int hp_center_actual_period(
    const HpContext& ctx,
    const HpC& c,
    int period,
    double& repeat_error
) {
    HpC z(ctx.prec);
    HpWork w(ctx.prec);
    HpReal n2(ctx.prec);
    repeat_error = 0.0;
    for (int n = 1; n <= period; ++n) {
        hp_step(z, c, w);
        if (period % n != 0) continue;
        hp_norm2(n2.v, z, w);
        if (mpfr_cmp(n2.v, ctx.eps2.v) <= 0) {
            mpfr_sqrt(n2.v, n2.v, MPFR_RNDN);
            repeat_error = mpfr_get_d(n2.v, MPFR_RNDN);
            return n;
        }
    }
    return -1;
}

struct HpMisiurewiczClass {
    bool is_center = false;
    bool is_misiurewicz = false;
    int preperiod = -1;
    int period = -1;
    double repeat_error = 0.0;
};

// Classify the critical orbit of c under the requested (m, p): find the
// minimal period among divisors of p, the minimal preperiod for that period,
// and detect center degeneracy (|z_q| ≈ 0 for some q ≤ m + p).
HpMisiurewiczClass hp_classify_misiurewicz(
    const HpContext& ctx,
    const HpC& c,
    int preperiod,
    int period
) {
    HpMisiurewiczClass out;
    const int total = preperiod + period;

    std::vector<std::unique_ptr<HpC>> orbit;
    orbit.reserve(static_cast<size_t>(total) + 1);
    HpWork w(ctx.prec);
    HpReal n2(ctx.prec);
    HpC diff(ctx.prec);

    HpC z(ctx.prec);
    orbit.push_back(std::make_unique<HpC>(ctx.prec));  // z_0 = 0
    for (int n = 1; n <= total; ++n) {
        hp_step(z, c, w);
        auto stored = std::make_unique<HpC>(ctx.prec);
        hp_set(*stored, z);
        orbit.push_back(std::move(stored));

        hp_norm2(n2.v, z, w);
        if (mpfr_cmp(n2.v, ctx.eps2.v) <= 0) {
            // The critical orbit returns to 0: this is a hyperbolic center.
            out.is_center = true;
            out.preperiod = 0;
            out.period = n;
            mpfr_sqrt(n2.v, n2.v, MPFR_RNDN);
            out.repeat_error = mpfr_get_d(n2.v, MPFR_RNDN);
            return out;
        }
    }

    // Minimal period among divisors of p, measured at index m.
    int actual_period = -1;
    for (int q = 1; q <= period; ++q) {
        if (period % q != 0) continue;
        hp_sub(diff, *orbit[static_cast<size_t>(preperiod + q)], *orbit[static_cast<size_t>(preperiod)]);
        hp_norm2(n2.v, diff, w);
        if (mpfr_cmp(n2.v, ctx.eps2.v) <= 0) {
            actual_period = q;
            mpfr_sqrt(n2.v, n2.v, MPFR_RNDN);
            out.repeat_error = mpfr_get_d(n2.v, MPFR_RNDN);
            break;
        }
    }
    if (actual_period < 1) {
        return out;  // no repeat within tolerance
    }

    // Minimal preperiod for that period.
    int actual_preperiod = preperiod;
    for (int m = 0; m < preperiod; ++m) {
        if (m + actual_period > total) break;
        hp_sub(diff, *orbit[static_cast<size_t>(m + actual_period)], *orbit[static_cast<size_t>(m)]);
        hp_norm2(n2.v, diff, w);
        if (mpfr_cmp(n2.v, ctx.eps2.v) <= 0) {
            actual_preperiod = m;
            break;
        }
    }

    if (actual_preperiod == 0) {
        // Periodic from z_0 = 0 without |z_q| ≈ 0 shouldn't happen; treat as
        // center-degenerate to stay conservative.
        out.is_center = true;
        out.preperiod = 0;
        out.period = actual_period;
        return out;
    }
    out.is_misiurewicz = true;
    out.preperiod = actual_preperiod;
    out.period = actual_period;
    return out;
}

// ---------------------------------------------------------------------------
// Result assembly.
// ---------------------------------------------------------------------------

std::string hp_format_id(
    const HpContext& ctx,
    SpecialPointKind kind,
    int preperiod,
    int period,
    const HpC& c
) {
    std::string id = kind == SpecialPointKind::HyperbolicCenter ? "center" : "misiurewicz";
    id += "-m" + std::to_string(preperiod) + "-p" + std::to_string(period) + "-";
    id += hp_to_string(c.re, ctx.id_digits);
    const std::string im = hp_to_string(c.im, ctx.id_digits);
    if (!im.empty() && im[0] != '-') id += "+";
    id += im + "i";
    return id;
}

SpecialPointResult hp_make_result(
    const HpContext& ctx,
    SpecialPointKind kind,
    int preperiod,
    int period,
    const HpC& c,
    const HpNewtonOutcome& newton,
    bool include_variants
) {
    SpecialPointResult out;
    out.kind = kind;
    out.preperiod = preperiod;
    out.period = period;
    out.re = mpfr_get_d(c.re, MPFR_RNDN);
    out.im = mpfr_get_d(c.im, MPFR_RNDN);
    out.re_str = hp_to_string(c.re, ctx.out_digits);
    out.im_str = hp_to_string(c.im, ctx.out_digits);
    out.prec_bits = ctx.bits;
    out.id = hp_format_id(ctx, kind, preperiod, period, c);
    out.converged = newton.converged;
    out.residual = newton.residual;
    out.newton_iterations = newton.iterations;
    if (include_variants) {
        // Double-precision variant orbits are meaningless at these depths;
        // report the Mandelbrot family the solve actually ran on.
        VariantExistence mandel;
        mandel.variant_name = "Mandelbrot";
        mandel.exists = true;
        mandel.same_orbit_as_mandelbrot = true;
        mandel.actual_preperiod = preperiod;
        mandel.actual_period = period;
        mandel.repeat_error = newton.residual;
        mandel.reason = "hp_solve";
        out.variants.push_back(mandel);
    }
    return out;
}

bool hp_better_fallback(const SpecialPointResult& candidate, const SpecialPointResult& current, bool has_current) {
    if (!candidate.actual.found_repeat) return false;
    if (!has_current) return true;
    if (candidate.actual.period != current.actual.period) return candidate.actual.period > current.actual.period;
    if (candidate.actual.preperiod != current.actual.preperiod) return candidate.actual.preperiod > current.actual.preperiod;
    return candidate.residual < current.residual;
}

std::vector<std::pair<double, double>> hp_seed_offsets(const HpContext& ctx) {
    return {
        {0.0, 0.0},
        {-ctx.half_w * 0.5, -ctx.half_h * 0.5},
        {ctx.half_w * 0.5, -ctx.half_h * 0.5},
        {-ctx.half_w * 0.5, ctx.half_h * 0.5},
        {ctx.half_w * 0.5, ctx.half_h * 0.5},
    };
}

void hp_seed_at(const HpContext& ctx, HpC& seed, double off_re, double off_im) {
    mpfr_set_d(seed.re, off_re, MPFR_RNDN);
    mpfr_add(seed.re, seed.re, ctx.center.re, MPFR_RNDN);
    mpfr_set_d(seed.im, off_im, MPFR_RNDN);
    mpfr_add(seed.im, seed.im, ctx.center.im, MPFR_RNDN);
}

// ---------------------------------------------------------------------------
// Search drivers.
// ---------------------------------------------------------------------------

SpecialPointSearchResponse hp_search_centers(
    const SpecialPointSearchRequest& req,
    const SpecialPointSearchProgressCallback& progress
) {
    SpecialPointSearchResponse resp;
    resp.status = "searching";
    HpContext ctx(req.viewport);
    HpWork w(ctx.prec);

    const std::vector<int> candidates =
        hp_ball_periods(ctx, req.period_min, req.period_max, HP_MAX_BALL_CANDIDATES);
    const auto seeds = hp_seed_offsets(ctx);

    SpecialPointResult best_fallback;
    bool has_fallback = false;
    bool accepted_any = false;
    long long step_budget = HP_CENTER_STEP_BUDGET;
    bool budget_exhausted = false;

    int candidate_index = 0;
    for (int candidate : candidates) {
        ++candidate_index;
        if (progress && !progress(candidate, candidate_index,
                                  static_cast<int>(candidates.size()),
                                  resp.accepted_count, resp.seed_count)) {
            resp.status = "cancelled";
            return resp;
        }
        if (step_budget <= 0) {
            budget_exhausted = true;
            break;
        }

        for (const auto& offset : seeds) {
            HpC c(ctx.prec);
            hp_seed_at(ctx, c, offset.first, offset.second);
            ++resp.seed_count;

            const HpNewtonOutcome newton = hp_newton(
                ctx, c, candidate, HP_NEWTON_MAX_ITER,
                [candidate](const HpC& cc, HpC& f, HpC& df, HpWork& ww) {
                    hp_center_f_df(cc, candidate, f, df, ww);
                });
            step_budget -= newton.steps;
            if (!newton.converged) {
                ++resp.rejected_count;
                continue;
            }
            ++resp.newton_success_count;

            double repeat_error = 0.0;
            const int actual = hp_center_actual_period(ctx, c, candidate, repeat_error);
            if (actual < 1) {
                ++resp.rejected_count;
                continue;
            }

            SpecialPointResult root = hp_make_result(
                ctx, SpecialPointKind::HyperbolicCenter, 0, actual, c, newton,
                req.include_variant_compatibility);
            root.actual.found_repeat = true;
            root.actual.is_center = true;
            root.actual.preperiod = 0;
            root.actual.period = actual;
            root.actual.repeat_error = repeat_error;
            root.visible = hp_in_viewport(ctx, c, w);

            const bool in_range = actual >= req.period_min && actual <= req.period_max;
            if (in_range && (!req.visible_only || root.visible)) {
                root.accepted = true;
                root.reason = "ok";
                resp.points.push_back(std::move(root));
                accepted_any = true;
                break;
            }

            ++resp.rejected_count;
            root.accepted = false;
            root.fallback = true;
            root.reason = root.visible ? "period_out_of_range" : "outside_viewport";
            if (hp_better_fallback(root, best_fallback, has_fallback)) {
                best_fallback = std::move(root);
                has_fallback = true;
            }
        }
        if (accepted_any) break;
    }

    resp.status = "completed";
    if (resp.points.empty() && has_fallback) {
        best_fallback.reason = "fallback_highest_actual_period";
        resp.points.push_back(std::move(best_fallback));
    }
    resp.accepted_count = static_cast<int>(std::count_if(
        resp.points.begin(), resp.points.end(), [](const auto& p) { return p.accepted; }));
    resp.fallback_count = static_cast<int>(std::count_if(
        resp.points.begin(), resp.points.end(), [](const auto& p) { return p.fallback; }));

    const std::string prec_note =
        "mpfr" + std::to_string(ctx.bits) + " ball-period deep solve";
    if (resp.accepted_count > 0) {
        resp.warning = prec_note + "; stopped at the first nucleus matching the viewport";
    } else if (resp.fallback_count > 0) {
        resp.warning = prec_note + "; no nucleus matched the viewport, showing the best classified candidate";
    } else if (candidates.empty()) {
        resp.warning = prec_note + "; no candidate period <= periodMax found for this viewport, increase periodMax";
    } else {
        resp.warning = prec_note + "; Newton did not converge on any candidate period";
    }
    if (budget_exhausted) {
        resp.warning += "; step budget exhausted before trying all candidate periods";
    }
    return resp;
}

SpecialPointSearchResponse hp_search_misiurewicz(
    const SpecialPointSearchRequest& req,
    const SpecialPointSearchProgressCallback& progress
) {
    SpecialPointSearchResponse resp;
    resp.status = "searching";
    HpContext ctx(req.viewport);
    HpWork w(ctx.prec);

    SpecialPointResult best_fallback;
    bool has_fallback = false;
    long long step_budget = HP_MISIUREWICZ_STEP_BUDGET;
    bool budget_exhausted = false;

    const int task_count =
        (req.preperiod_max - req.preperiod_min + 1) * (req.period_max - req.period_min + 1);
    int task_index = 0;
    bool accepted_any = false;

    for (int m = req.preperiod_min; m <= req.preperiod_max && !accepted_any && !budget_exhausted; ++m) {
        for (int p = req.period_min; p <= req.period_max; ++p) {
            ++task_index;
            if (progress && !progress(p, task_index, task_count,
                                      resp.accepted_count, resp.seed_count)) {
                resp.status = "cancelled";
                return resp;
            }
            if (step_budget <= 0) {
                budget_exhausted = true;
                break;
            }

            HpC c(ctx.prec);
            hp_set(c, ctx.center);
            ++resp.seed_count;

            const HpNewtonOutcome newton = hp_newton(
                ctx, c, m + p, HP_NEWTON_MAX_ITER,
                [m, p](const HpC& cc, HpC& f, HpC& df, HpWork& ww) {
                    hp_misiurewicz_f_df(cc, m, p, f, df, ww);
                });
            step_budget -= newton.steps;
            if (!newton.converged) {
                ++resp.rejected_count;
                continue;
            }
            ++resp.newton_success_count;

            const HpMisiurewiczClass cls = hp_classify_misiurewicz(ctx, c, m, p);
            const bool matches = cls.is_misiurewicz && cls.preperiod == m && cls.period == p;

            SpecialPointResult root = hp_make_result(
                ctx, SpecialPointKind::Misiurewicz, m, p, c, newton,
                req.include_variant_compatibility);
            root.actual.found_repeat = cls.is_center || cls.is_misiurewicz;
            root.actual.is_center = cls.is_center;
            root.actual.is_misiurewicz = cls.is_misiurewicz;
            root.actual.preperiod = cls.preperiod;
            root.actual.period = cls.period;
            root.actual.repeat_error = cls.repeat_error;
            root.visible = hp_in_viewport(ctx, c, w);

            if (matches && (!req.visible_only || root.visible)) {
                root.accepted = true;
                root.reason = "ok";
                resp.points.push_back(std::move(root));
                accepted_any = true;
                break;
            }

            ++resp.rejected_count;
            if (!root.actual.found_repeat) continue;
            root.accepted = false;
            root.fallback = true;
            root.kind = cls.is_center ? SpecialPointKind::HyperbolicCenter : SpecialPointKind::Misiurewicz;
            root.preperiod = std::max(0, cls.preperiod);
            root.period = std::max(1, cls.period);
            root.id = hp_format_id(ctx, root.kind, root.preperiod, root.period, c);
            root.reason = "fallback_highest_actual_period";
            if ((!req.visible_only || root.visible) && hp_better_fallback(root, best_fallback, has_fallback)) {
                best_fallback = std::move(root);
                has_fallback = true;
            }
        }
    }

    resp.status = "completed";
    if (resp.points.empty() && has_fallback) {
        resp.points.push_back(std::move(best_fallback));
    }
    resp.accepted_count = static_cast<int>(std::count_if(
        resp.points.begin(), resp.points.end(), [](const auto& p) { return p.accepted; }));
    resp.fallback_count = static_cast<int>(std::count_if(
        resp.points.begin(), resp.points.end(), [](const auto& p) { return p.fallback; }));

    const std::string prec_note = "mpfr" + std::to_string(ctx.bits) + " deep Misiurewicz solve";
    if (resp.accepted_count > 0) {
        resp.warning = prec_note + "; stopped at the first exact match from the viewport center";
    } else if (resp.fallback_count > 0) {
        resp.warning = prec_note + "; no exact match, showing the highest-period classified candidate";
    } else {
        resp.warning = prec_note + "; no matching local Misiurewicz point found from the current center";
    }
    if (budget_exhausted) {
        resp.warning += "; step budget exhausted before covering the requested preperiod/period ranges";
    }
    return resp;
}

} // namespace

bool special_points_hp_available() { return true; }

SpecialPointSearchResponse search_special_points_hp(
    const SpecialPointSearchRequest& req,
    const SpecialPointSearchProgressCallback& progress
) {
    if (!req.viewport.enabled) {
        throw std::runtime_error("viewport is required for special point search");
    }
    if (req.kind == SpecialPointKind::HyperbolicCenter) {
        return hp_search_centers(req, progress);
    }
    return hp_search_misiurewicz(req, progress);
}

#endif // FSD_HAS_MPFR

} // namespace fsd::compute
