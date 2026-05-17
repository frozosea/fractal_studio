// compute/variants.hpp
//
// The 16 Mandelbrot-family variant step functions, tag-dispatched so the
// compiler inlines the chosen body into the escape-time loop. Math ported
// verbatim from C_mandelbrot/Mandelbrot_python_ln.c and the legacy managed map
// renderer in fractal_studio/backend/src/adapters/legacy_map_adapter.cpp.
//
// Variant catalog (see README.md for full descriptions):
//   mandelbrot   — z² + c                                (standard)
//   tricorn      — conj(z²) + c                          (Tricorn / Mandelbar)
//   burning_ship — (|Re z| + |Im z|·i)² + c             (Burning Ship)
//   celtic       — (Re z + |Im z|·i)² + c               (Perpendicular Burning Ship; legacy id)
//   heart        — (|Re z| − Im z·i)² + c               (Perpendicular Mandelbrot / Heart)
//   buffalo      — z² → |Re(z²)| + Im(z²)·i + c        (Celtic; legacy id)
//   perp_buffalo — z² → |Re(z²)| − Im(z²)·i + c        (Mandelbar Celtic; legacy id)
//   celtic_ship  — z² → |Re(z²)| + |Im(z²)|·i + c      (Buffalo; legacy id)
//   mandelceltic — (Re+|Im|·i)² → |Re|+Im·i + c         (Perpendicular Buffalo; legacy id)
//   perp_ship    — (|Re|+Im·i)² → |Re|−Im·i + c         (Perpendicular Celtic; legacy id)
//   sin_z        — sin(z) + c
//   cos_z        — cos(z) + c
//   exp_z        — exp(z) + c
//   sinh_z       — sinh(z) + c
//   cosh_z       — cosh(z) + c
//   tan_z        — tan(z) + c

#pragma once

#include "complex.hpp"
#include <cmath>
#include <type_traits>

namespace fsd::compute {

enum class Variant {
    Mandelbrot  = 0,  // z² + c
    Tri         = 1,  // conj(z²) + c                       (Tricorn / Mandelbar)
    Boat        = 2,  // (|Re| + |Im|·i)² + c               (Burning Ship)
    Duck        = 3,  // (Re + |Im|·i)² + c                 (Perpendicular Burning Ship)
    Bell        = 4,  // (|Re| − Im·i)² + c                 (Perpendicular Mandelbrot / Heart)
    Fish        = 5,  // z²→|Re(z²)|+Im(z²)·i+c            (Celtic)
    Vase        = 6,  // z²→|Re(z²)|−Im(z²)·i+c            (Mandelbar Celtic)
    Bird        = 7,  // z²→|Re(z²)|+|Im(z²)|·i+c          (Buffalo)
    Mask        = 8,  // (Re+|Im|·i)²→|Re|+Im·i+c          (Perpendicular Buffalo)
    Ship        = 9,  // (|Re|+Im·i)²→|Re|−Im·i+c          (Perpendicular Celtic)
    SinZ        = 10, // sin(z) + c
    CosZ        = 11, // cos(z) + c
    ExpZ        = 12, // exp(z) + c
    SinhZ       = 13, // sinh(z) + c
    CoshZ       = 14, // cosh(z) + c
    TanZ        = 15, // tan(z) + c
    Custom      = 100, // user-compiled formula via dlopen; step fn in MapParams::custom_step_fn
};

// ─── complex trig helpers (double only) ──────────────────────────────────────
// All identities use real-valued cmath functions to avoid <complex> dependency.

inline Cx<double> cx_sin(Cx<double> z) {
    // sin(x+iy) = sin(x)cosh(y) + i·cos(x)sinh(y)
    return { std::sin(z.re) * std::cosh(z.im),
             std::cos(z.re) * std::sinh(z.im) };
}
inline Cx<double> cx_cos(Cx<double> z) {
    // cos(x+iy) = cos(x)cosh(y) − i·sin(x)sinh(y)
    return { std::cos(z.re) * std::cosh(z.im),
            -std::sin(z.re) * std::sinh(z.im) };
}
inline Cx<double> cx_exp(Cx<double> z) {
    // exp(x+iy) = e^x·(cos(y) + i·sin(y))
    const double ex = std::exp(z.re);
    return { ex * std::cos(z.im), ex * std::sin(z.im) };
}
inline Cx<double> cx_sinh(Cx<double> z) {
    // sinh(x+iy) = sinh(x)cos(y) + i·cosh(x)sin(y)
    return { std::sinh(z.re) * std::cos(z.im),
             std::cosh(z.re) * std::sin(z.im) };
}
inline Cx<double> cx_cosh(Cx<double> z) {
    // cosh(x+iy) = cosh(x)cos(y) + i·sinh(x)sin(y)
    return { std::cosh(z.re) * std::cos(z.im),
             std::sinh(z.re) * std::sin(z.im) };
}
inline Cx<double> cx_tan(Cx<double> z) {
    // tan(z) = sin(z) / cos(z), both complex.
    // Numerically stable form: tan(z) = sin(2x)/(cos(2x)+cosh(2y))
    //                              + i·sinh(2y)/(cos(2x)+cosh(2y))
    const double denom = std::cos(2.0 * z.re) + std::cosh(2.0 * z.im);
    if (denom == 0.0) return { 0.0, 0.0 };
    return { std::sin(2.0 * z.re) / denom,
             std::sinh(2.0 * z.im) / denom };
}

// Helper to apply a trig function when S may not be double (Fx64 fallback).
// Casts to Cx<double>, applies fn, casts result back to Cx<S>.
template <typename S>
Cx<S> apply_trig(Cx<S> z, Cx<S> c, Cx<double>(*fn)(Cx<double>)) {
    Cx<double> zd{ static_cast<double>(z.re), static_cast<double>(z.im) };
    Cx<double> cd{ static_cast<double>(c.re), static_cast<double>(c.im) };
    Cx<double> rd = fn(zd);
    rd.re += cd.re;
    rd.im += cd.im;
    return { scalar_from_double<S>(rd.re), scalar_from_double<S>(rd.im) };
}

inline const char* variant_name(Variant v) {
    switch (v) {
        case Variant::Mandelbrot: return "mandelbrot";
        case Variant::Tri:        return "tricorn";
        case Variant::Boat:       return "burning_ship";
        case Variant::Duck:       return "celtic";
        case Variant::Bell:       return "heart";
        case Variant::Fish:       return "buffalo";
        case Variant::Vase:       return "perp_buffalo";
        case Variant::Bird:       return "celtic_ship";
        case Variant::Mask:       return "mandelceltic";
        case Variant::Ship:       return "perp_ship";
        case Variant::SinZ:       return "sin_z";
        case Variant::CosZ:       return "cos_z";
        case Variant::ExpZ:       return "exp_z";
        case Variant::SinhZ:      return "sinh_z";
        case Variant::CoshZ:      return "cosh_z";
        case Variant::TanZ:       return "tan_z";
        case Variant::Custom:     return "custom";
    }
    return "mandelbrot";
}

inline bool variant_from_name(const char* name, Variant& out) {
    struct Entry { const char* n; Variant v; };
    static constexpr Entry table[] = {
        {"mandelbrot",  Variant::Mandelbrot},
        {"tricorn",     Variant::Tri},
        {"burning_ship",Variant::Boat},
        {"celtic",      Variant::Duck},
        {"heart",       Variant::Bell},
        {"buffalo",     Variant::Fish},
        {"perp_buffalo",Variant::Vase},
        {"celtic_ship", Variant::Bird},
        {"mandelceltic",Variant::Mask},
        {"perp_ship",   Variant::Ship},
        {"sin_z",       Variant::SinZ},
        {"cos_z",       Variant::CosZ},
        {"exp_z",       Variant::ExpZ},
        {"sinh_z",      Variant::SinhZ},
        {"cosh_z",      Variant::CoshZ},
        {"tan_z",       Variant::TanZ},
        // Legacy aliases (keep old names working)
        {"tri",         Variant::Tri},
        {"boat",        Variant::Boat},
        {"duck",        Variant::Duck},
        {"bell",        Variant::Bell},
        {"fish",        Variant::Fish},
        {"vase",        Variant::Vase},
        {"bird",        Variant::Bird},
        {"mask",        Variant::Mask},
        {"ship",        Variant::Ship},
    };
    for (const auto& e : table) {
        const char* a = e.n;
        const char* b = name;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (*a == 0 && *b == 0) { out = e.v; return true; }
    }
    return false;
}

// Returns true for variants that require scalar (fp64) fallback in
// vectorised paths (AVX-512, CUDA). Trig variants use std::cmath and cannot
// easily be vectorised without a dedicated SVML/libm port.
inline bool variant_needs_scalar_fallback(Variant v) {
    switch (v) {
        case Variant::SinZ:
        case Variant::CosZ:
        case Variant::ExpZ:
        case Variant::SinhZ:
        case Variant::CoshZ:
        case Variant::TanZ:
            return true;
        default:
            return false;
    }
}

inline bool variant_is_transcendental(Variant v) {
    switch (v) {
        case Variant::SinZ:
        case Variant::CosZ:
        case Variant::ExpZ:
        case Variant::SinhZ:
        case Variant::CoshZ:
        case Variant::TanZ:
            return true;
        default:
            return false;
    }
}

template <Variant V>
constexpr bool variant_is_transcendental_v() {
    return V == Variant::SinZ
        || V == Variant::CosZ
        || V == Variant::ExpZ
        || V == Variant::SinhZ
        || V == Variant::CoshZ
        || V == Variant::TanZ;
}

// Escape policy:
//   - quadratic/folded polynomial variants use the classic radius test |z|²>R².
//   - transcendental variants do not have a single useful escape circle. Their
//     sin/cos/sinh/cosh/exp/tan formulas grow through individual components or
//     poles, so the kernels use max(|Re|, |Im|)>R plus non-finite detection.
inline double variant_default_bailout(Variant v) {
    return variant_is_transcendental(v) ? 64.0 : 2.0;
}

inline double variant_default_bailout_sq(Variant v) {
    const double r = variant_default_bailout(v);
    return r * r;
}

inline bool variant_supports_axis_transition(Variant v) {
    const int id = static_cast<int>(v);
    return id >= 0 && id <= 9;
}

inline bool variant_transition_post_abs_real(Variant v) {
    switch (v) {
        case Variant::Fish:
        case Variant::Vase:
        case Variant::Bird:
        case Variant::Mask:
        case Variant::Ship:
            return true;
        default:
            return false;
    }
}

inline double variant_transition_real_projection(Variant v, double x2, double axis2) {
    const double q = x2 - axis2;
    return variant_transition_post_abs_real(v) ? std::fabs(q) : q;
}

inline double variant_transition_imag_projection(Variant v, double x, double axis) {
    switch (v) {
        case Variant::Tri:
        case Variant::Vase:
            return -2.0 * x * axis;
        case Variant::Boat:
        case Variant::Bird:
            return 2.0 * std::fabs(x * axis);
        case Variant::Duck:
        case Variant::Mask:
            return 2.0 * x * std::fabs(axis);
        case Variant::Bell:
        case Variant::Ship:
            return -2.0 * std::fabs(x) * axis;
        case Variant::Mandelbrot:
        case Variant::Fish:
        default:
            return 2.0 * x * axis;
    }
}

// Tag-dispatched step. The compiler collapses the switch on V away when V is a
// template constant, so each instantiation gets a tight loop for its variant.
template <Variant V, typename S>
inline Cx<S> variant_step(Cx<S> z, const Cx<S>& c) {
    if constexpr (V == Variant::Mandelbrot) {
        return z.sqr() + c;
    } else if constexpr (V == Variant::Tri) {
        return conj_sqr(z) + c;
    } else if constexpr (V == Variant::Boat) {
        Cx<S> w{scalar_abs(z.re), scalar_abs(z.im)};
        return w.sqr() + c;
    } else if constexpr (V == Variant::Duck) {
        Cx<S> w{z.re, scalar_abs(z.im)};
        return w.sqr() + c;
    } else if constexpr (V == Variant::Bell) {
        Cx<S> w{scalar_abs(z.re), -z.im};
        return w.sqr() + c;
    } else if constexpr (V == Variant::Fish) {
        Cx<S> w = z.sqr();
        w.re = scalar_abs(w.re);
        return w + c;
    } else if constexpr (V == Variant::Vase) {
        Cx<S> w = z.sqr();
        w.re = scalar_abs(w.re);
        w.im = -w.im;
        return w + c;
    } else if constexpr (V == Variant::Bird) {
        Cx<S> w = z.sqr();
        w.re = scalar_abs(w.re);
        w.im = scalar_abs(w.im);
        return w + c;
    } else if constexpr (V == Variant::Mask) {
        Cx<S> w{z.re, scalar_abs(z.im)};
        w = w.sqr();
        w.re = scalar_abs(w.re);
        return w + c;
    } else if constexpr (V == Variant::Ship) {
        Cx<S> w{scalar_abs(z.re), z.im};
        w = w.sqr();
        w.re = scalar_abs(w.re);
        w.im = -w.im;
        return w + c;
    } else if constexpr (V == Variant::SinZ) {
        return apply_trig(z, c, cx_sin);
    } else if constexpr (V == Variant::CosZ) {
        return apply_trig(z, c, cx_cos);
    } else if constexpr (V == Variant::ExpZ) {
        return apply_trig(z, c, cx_exp);
    } else if constexpr (V == Variant::SinhZ) {
        return apply_trig(z, c, cx_sinh);
    } else if constexpr (V == Variant::CoshZ) {
        return apply_trig(z, c, cx_cosh);
    } else if constexpr (V == Variant::TanZ) {
        return apply_trig(z, c, cx_tan);
    }
}

// Cached quadratic step for z^2-family variants.  x2/y2 must be the current
// x*x and y*y in the same scalar format as x/y.  Folded variants still
// recompute xy with their own abs/sign rule, while reusing x2/y2 because
// abs(x)^2 == x^2 and (-y)^2 == y^2.
template <Variant V, typename S>
inline void step_cached(
    S x,
    S y,
    S x2,
    S y2,
    S cx,
    S cy,
    S& nx,
    S& ny
) {
    static_assert(!variant_is_transcendental_v<V>(),
        "step_cached only supports quadratic variants.");

    const S two = S(2);
    S sq_re = x2 - y2;
    S sq_im{};

    if constexpr (V == Variant::Mandelbrot) {
        sq_im = two * x * y;
        nx = sq_re + cx;
        ny = sq_im + cy;
    } else if constexpr (V == Variant::Tri) {
        sq_im = two * x * y;
        nx = sq_re + cx;
        ny = -sq_im + cy;
    } else if constexpr (V == Variant::Boat) {
        sq_im = two * scalar_abs(x) * scalar_abs(y);
        nx = sq_re + cx;
        ny = sq_im + cy;
    } else if constexpr (V == Variant::Duck) {
        sq_im = two * x * scalar_abs(y);
        nx = sq_re + cx;
        ny = sq_im + cy;
    } else if constexpr (V == Variant::Bell) {
        sq_im = two * scalar_abs(x) * (-y);
        nx = sq_re + cx;
        ny = sq_im + cy;
    } else if constexpr (V == Variant::Fish) {
        sq_im = two * x * y;
        nx = scalar_abs(sq_re) + cx;
        ny = sq_im + cy;
    } else if constexpr (V == Variant::Vase) {
        sq_im = two * x * y;
        nx = scalar_abs(sq_re) + cx;
        ny = -sq_im + cy;
    } else if constexpr (V == Variant::Bird) {
        sq_im = two * x * y;
        nx = scalar_abs(sq_re) + cx;
        ny = scalar_abs(sq_im) + cy;
    } else if constexpr (V == Variant::Mask) {
        sq_im = two * x * scalar_abs(y);
        nx = scalar_abs(sq_re) + cx;
        ny = sq_im + cy;
    } else if constexpr (V == Variant::Ship) {
        sq_im = two * scalar_abs(x) * y;
        nx = scalar_abs(sq_re) + cx;
        ny = -sq_im + cy;
    }
}

} // namespace fsd::compute
