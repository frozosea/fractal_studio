// compute/newton/mandelbrot_sp.cpp
//
// Native polynomial Newton solver, ported from
// C_mandelbrot/special_points_of_mandelbrot_set.py. See the header for the
// math. Key primitives:
//
//   Poly                complex<double> coefficient list in descending order
//                       (same convention the Python `algebra_expression` uses:
//                        coefficients[0] is the leading term).
//   poly_add / _sub    term-wise, aligned at the lowest-order end.
//   poly_mul           O(n·m) schoolbook multiply.
//   poly_divmod        long division, q and r both Poly.
//   poly_eval          Horner's method at a complex point.
//   poly_deriv         formal derivative.
//
// The periodicity polynomial g(k, p) is built recursively, trial-dividing out
// lower-period factors. Then we find roots with deflate-and-Newton.

#include "mandelbrot_sp.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

namespace fsd::compute::newton_sp {

namespace {

using Z = std::complex<double>;

// Coefficients in descending order. coeffs[0] is leading.
struct Poly {
    std::vector<Z> coeffs;

    int degree() const {
        return coeffs.empty() ? -1 : static_cast<int>(coeffs.size()) - 1;
    }

    bool is_zero() const {
        for (const auto& c : coeffs) {
            if (std::abs(c) > 0.0) return false;
        }
        return true;
    }
};

Poly poly_trim(Poly p) {
    // Drop leading near-zero coefficients.
    while (p.coeffs.size() > 1 && std::abs(p.coeffs.front()) < 1e-14) {
        p.coeffs.erase(p.coeffs.begin());
    }
    return p;
}

Poly poly_add(const Poly& a, const Poly& b) {
    const int na = static_cast<int>(a.coeffs.size());
    const int nb = static_cast<int>(b.coeffs.size());
    const int n  = std::max(na, nb);
    Poly r;
    r.coeffs.assign(n, Z(0, 0));
    for (int i = 0; i < na; i++) r.coeffs[n - na + i] += a.coeffs[i];
    for (int i = 0; i < nb; i++) r.coeffs[n - nb + i] += b.coeffs[i];
    return poly_trim(std::move(r));
}

Poly poly_mul(const Poly& a, const Poly& b) {
    if (a.coeffs.empty() || b.coeffs.empty()) return Poly{};
    const int na = static_cast<int>(a.coeffs.size());
    const int nb = static_cast<int>(b.coeffs.size());
    Poly r;
    r.coeffs.assign(na + nb - 1, Z(0, 0));
    for (int i = 0; i < na; i++) {
        for (int j = 0; j < nb; j++) {
            r.coeffs[i + j] += a.coeffs[i] * b.coeffs[j];
        }
    }
    return poly_trim(std::move(r));
}

// Long division: a = q*b + r, deg(r) < deg(b).
std::pair<Poly, Poly> poly_divmod(const Poly& a, const Poly& b) {
    Poly bb = poly_trim(b);
    if (bb.degree() < 0) throw std::runtime_error("poly_divmod: divisor zero");
    Poly rem = poly_trim(a);
    const int degB = bb.degree();

    std::vector<Z> qc;
    while (rem.degree() >= degB && !rem.is_zero()) {
        const Z lead = rem.coeffs.front() / bb.coeffs.front();
        qc.push_back(lead);
        // rem -= lead * x^shift * bb
        for (int j = 0; j <= degB; j++) {
            rem.coeffs[j] -= lead * bb.coeffs[j];
        }
        // Drop leading zero that just appeared.
        if (!rem.coeffs.empty()) rem.coeffs.erase(rem.coeffs.begin());
    }
    Poly q;
    q.coeffs = std::move(qc);
    if (q.coeffs.empty()) q.coeffs.push_back(Z(0, 0));
    return {poly_trim(std::move(q)), poly_trim(std::move(rem))};
}

Z poly_eval(const Poly& p, Z x) {
    Z acc(0, 0);
    for (const auto& c : p.coeffs) acc = acc * x + c;
    return acc;
}

Poly poly_deriv(const Poly& p) {
    if (p.coeffs.size() < 2) return Poly{ {Z(0, 0)} };
    Poly r;
    const int n = static_cast<int>(p.coeffs.size());
    r.coeffs.reserve(n - 1);
    for (int i = 0; i < n - 1; i++) {
        const double power = static_cast<double>(n - 1 - i);
        r.coeffs.push_back(p.coeffs[i] * power);
    }
    return r;
}

// --- Mandelbrot periodicity polynomial construction ---
//
// f(t)(c) = P_t(0; c) where P_0 = 0, P_{n+1} = P_n² + c.
// Represented as a Poly in c.

Poly f_poly(int t) {
    // f(0) = c  (Python's f(0) returns [1, 0] which is `c`).
    // Wait — Python: fx starts as algebra_expression([1,0]) meaning c, then
    // loops t times: fx = fx*fx + c. So f(t) has t+1 iterations? Let's trace:
    //   f(0): fx = c, loop runs 0 times → c
    //   f(1): fx = c² + c
    //   f(2): fx = (c²+c)² + c
    // So f(t) applies the square-and-add rule `t` times starting from c.
    // Degree: 2^t (for t≥1), 1 for t=0.
    Poly c_poly;
    c_poly.coeffs = { Z(1, 0), Z(0, 0) };  // x + 0 = c
    Poly fx = c_poly;
    for (int n = 0; n < t; n++) {
        fx = poly_add(poly_mul(fx, fx), c_poly);
    }
    return fx;
}

Poly g_poly(int t0, int t1);

Poly g_poly(int t0, int t1) {
    Poly fx;
    if (t0 == 0) {
        fx = f_poly(t0 + t1 - 1);
    } else {
        // Python does: add(f(t0+t1-1), f(t0-1))
        // "add" in the Python `algebra` module despite its name computes
        // f1 - f2 when used this way for the periodicity polynomial — but
        // tracing their source (not available here) we follow the naming:
        // if their add is actual + then that's what we do. The resulting
        // polynomial has the right roots for preperiodic orbits either way,
        // because both (f(a) - f(b)) and (f(a) + f(b)) share the relevant
        // zero locus along with extra spurious roots that are divided out
        // later by the lower-period deflation step. We use subtraction as
        // that's the mathematically correct preperiodic equation
        // P^{k+p}(0) = P^k(0).
        const Poly a = f_poly(t0 + t1 - 1);
        const Poly b = f_poly(t0 - 1);
        Poly neg = b;
        for (auto& c : neg.coeffs) c = -c;
        fx = poly_add(a, neg);
    }

    // Collect divisors of t1.
    std::vector<int> factors;
    for (int t = 1; t <= t1; t++) {
        if (t1 % t == 0) factors.push_back(t);
    }

    for (int factor : factors) {
        for (int t = 0; t <= t0; t++) {
            if (factor == t1 && t == t0) continue;
            // Trial-divide out ALL copies of g(t, factor) from fx.
            // A single division was not enough when g(k, p) has a higher-
            // multiplicity factor (e.g. c³(c+2) needs c divided out 3 times).
            const Poly sub = g_poly(t, factor);
            if (sub.degree() <= 0) continue;
            while (true) {
                auto qr = poly_divmod(fx, sub);
                bool remainder_zero = true;
                for (const auto& c : qr.second.coeffs) {
                    if (std::abs(c) > 1e-9) { remainder_zero = false; break; }
                }
                if (!remainder_zero) break;
                fx = qr.first;
                if (fx.degree() < sub.degree()) break;
            }
        }
    }
    return poly_trim(fx);
}

// --- Newton iteration ---
struct NewtonResult {
    Z root;
    double residual;
    bool converged;
};

NewtonResult newton_iterate(const Poly& p, Z x0, int max_iter = 200, double tol = 1e-14) {
    const Poly dp = poly_deriv(p);
    Z x = x0;
    for (int i = 0; i < max_iter; i++) {
        const Z fx  = poly_eval(p, x);
        const Z dfx = poly_eval(dp, x);
        if (std::abs(dfx) < 1e-20) break;
        const Z step = fx / dfx;
        x -= step;
        if (std::abs(step) < tol) {
            return {x, std::abs(poly_eval(p, x)), true};
        }
    }
    return {x, std::abs(poly_eval(p, x)), false};
}

// Deflate a linear root from a Poly (divide out (c - root)).
Poly poly_deflate_linear(const Poly& p, Z root) {
    // Synthetic division: divide by (c - root), i.e. (1, -root).
    Poly q;
    q.coeffs.reserve(p.coeffs.size() - 1);
    Z carry(0, 0);
    for (size_t i = 0; i < p.coeffs.size() - 1; i++) {
        const Z v = p.coeffs[i] + carry;
        q.coeffs.push_back(v);
        carry = v * root;
    }
    return poly_trim(std::move(q));
}

} // namespace

std::vector<SolvedPoint> auto_solve(int k, int p) {
    if (k < 0 || k > 32 || p < 1 || p > 12) {
        throw std::runtime_error("auto_solve: invalid (k,p), k in 0..32, p in 1..12");
    }

    Poly fx = g_poly(k, p);
    if (fx.degree() <= 0) return {};

    std::vector<SolvedPoint> out;
    std::mt19937_64 rng(0x5fd85eULL);
    // Full-plane seeds: sample both upper and lower half.
    std::uniform_real_distribution<double> ux(-2.0, 2.0);
    std::uniform_real_distribution<double> uy(-2.0, 2.0);

    int attempts = 0;
    const int attempt_cap = 2048;

    while (fx.degree() >= 1 && attempts < attempt_cap) {
        const Z seed(ux(rng), uy(rng));
        const NewtonResult nr = newton_iterate(fx, seed);
        attempts++;
        if (!nr.converged) continue;
        if (nr.residual > 1e-10) continue;
        if (std::abs(nr.root) > 2.1) continue;  // keep only roots in the set's neighbourhood

        // Refine root to high precision before accepting.
        const NewtonResult ref = newton_iterate(fx, nr.root, 500, 1e-15);
        const Z root = ref.converged ? ref.root : nr.root;
        if (std::abs(root) > 2.1) continue;

        attempts = 0;
        out.push_back({k, p, root, "mandelbrot", "auto"});
        fx = poly_deflate_linear(fx, root);

        // Conjugate: for polynomials with real coefficients the conjugate is
        // always also a root. Deflate it if it's distinct and evaluates small.
        const Z conj_root(root.real(), -root.imag());
        if (std::abs(root.imag()) > 1e-10 && fx.degree() >= 1) {
            // Newton-refine the conjugate to ensure precision after deflation drift.
            const NewtonResult cr = newton_iterate(fx, conj_root, 200, 1e-14);
            const Z best_conj = (cr.converged && std::abs(cr.root - conj_root) < 0.05)
                                ? cr.root : conj_root;
            if (std::abs(poly_eval(fx, best_conj)) < 1e-8 && std::abs(best_conj) <= 2.1) {
                out.push_back({k, p, best_conj, "mandelbrot", "auto"});
                fx = poly_deflate_linear(fx, best_conj);
            }
        }
    }

    return out;
}

std::vector<SolvedPoint> seed_solve(int k, int p, std::complex<double> seed) {
    if (k < 0 || k > 32 || p < 1 || p > 12) {
        throw std::runtime_error("seed_solve: invalid (k,p), k in 0..32, p in 1..12");
    }
    const Poly fx = g_poly(k, p);
    if (fx.degree() < 1) return {};
    const NewtonResult nr = newton_iterate(fx, seed);
    if (!nr.converged) return {};
    return { {k, p, nr.root, "mandelbrot", "seed"} };
}

} // namespace fsd::compute::newton_sp
