// compute/transition_kernel.cpp

#include "transition_kernel.hpp"
#include "transition_kernel_avx2.hpp"
#include "colorize.hpp"
#include "map_kernel.hpp"
#include "parallel.hpp"
#include "scalar.hpp"

#if defined(HAS_CUDA_KERNEL)
#  include "cuda/transition_kernel.cuh"
#endif

#ifdef _OPENMP
#  include <omp.h>
#endif

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <stdexcept>
#include <vector>

namespace fsd::compute {

namespace {

constexpr double PI  = 3.14159265358979323846264338327950288;
constexpr double TAU = 6.28318530717958647692528676655900576;
constexpr int THETA_SCALE = 1000;
constexpr int THETA_HALF_TURN_MDEG = 180 * THETA_SCALE;
constexpr int THETA_FULL_TURN_MDEG = 360 * THETA_SCALE;

enum class DirectSlice {
    None,
    From,
    FromFlipY,
    To,
    ToFlipY,
};

struct NormalizedTheta {
    double radians = 0.0;
    int milli_deg = 0;
};

int normalize_transition_milli_deg(long long milli_deg) {
    long long wrapped = (milli_deg + THETA_HALF_TURN_MDEG) % THETA_FULL_TURN_MDEG;
    if (wrapped < 0) wrapped += THETA_FULL_TURN_MDEG;
    wrapped -= THETA_HALF_TURN_MDEG;
    if (wrapped == -THETA_HALF_TURN_MDEG && milli_deg > 0) {
        wrapped = THETA_HALF_TURN_MDEG;
    }
    return static_cast<int>(wrapped);
}

NormalizedTheta normalize_transition_theta(const TransitionParams& p) {
    if (p.theta_milli_deg_set) {
        const int milli_deg = normalize_transition_milli_deg(p.theta_milli_deg);
        return {
            static_cast<double>(milli_deg) * PI / (180.0 * THETA_SCALE),
            milli_deg,
        };
    }

    double theta = p.theta;
    if (!std::isfinite(theta)) throw std::runtime_error("invalid transitionTheta");
    if (std::abs(theta) > TAU + 1e-6) {
        theta = theta * PI / 180.0;
    }
    theta = std::remainder(theta, TAU);
    const int milli_deg = normalize_transition_milli_deg(
        static_cast<long long>(std::llround(theta * 180.0 * THETA_SCALE / PI)));
    return {
        static_cast<double>(milli_deg) * PI / (180.0 * THETA_SCALE),
        milli_deg,
    };
}

DirectSlice direct_slice_for_milli_deg(int milli_deg) {
    switch (milli_deg) {
        case 0:
            return DirectSlice::From;
        case 90 * THETA_SCALE:
            return DirectSlice::To;
        case -90 * THETA_SCALE:
            return DirectSlice::ToFlipY;
        case 180 * THETA_SCALE:
        case -180 * THETA_SCALE:
            return DirectSlice::FromFlipY;
        default:
            return DirectSlice::None;
    }
}

inline bool transition_cancel_requested(const TransitionParams& p) {
    return p.base.should_cancel && p.base.should_cancel();
}

[[noreturn]] void throw_transition_cancelled() {
    throw std::runtime_error("cancelled");
}

inline bool mark_cancelled_if_requested(const TransitionParams& p, std::atomic<bool>& cancelled) {
    if (cancelled.load(std::memory_order_relaxed)) return true;
    if (transition_cancel_requested(p)) {
        cancelled.store(true, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void flip_field_output_y(FieldOutput& fo) {
    const int W = fo.width;
    const int H = fo.height;
    for (int top = 0, bot = H - 1; top < bot; ++top, --bot) {
        const size_t off_top = static_cast<size_t>(top) * W;
        const size_t off_bot = static_cast<size_t>(bot) * W;
        if (fo.metric == Metric::Escape) {
            std::swap_ranges(fo.iter_u32.begin() + off_top,
                             fo.iter_u32.begin() + off_top + W,
                             fo.iter_u32.begin() + off_bot);
            if (!fo.norm_f32.empty()) {
                std::swap_ranges(fo.norm_f32.begin() + off_top,
                                 fo.norm_f32.begin() + off_top + W,
                                 fo.norm_f32.begin() + off_bot);
            }
        } else {
            std::swap_ranges(fo.field_f64.begin() + off_top,
                             fo.field_f64.begin() + off_top + W,
                             fo.field_f64.begin() + off_bot);
        }
    }
}

std::string negate_decimal_string(std::string value) {
    if (value.empty()) return value;

    const size_t sign_pos = value.find_first_not_of(" \t\r\n");
    if (sign_pos == std::string::npos) return value;
    if (value[sign_pos] == '-') {
        value.erase(sign_pos, 1);
    } else if (value[sign_pos] == '+') {
        value[sign_pos] = '-';
    } else {
        value.insert(sign_pos, 1, '-');
    }
    return value;
}

inline void transition_viewport_point(
    const MapParams& b,
    int W,
    int H,
    int x,
    int y,
    double re_min,
    double im_max,
    double span_re,
    double span_im,
    bool has_rot,
    double cos_t,
    double sin_t,
    double& u,
    double& v
) {
    if (has_rot) {
        const double pixel_step = b.scale / static_cast<double>(H);
        const double dx = (static_cast<double>(x) + 0.5 - static_cast<double>(W) * 0.5) * pixel_step;
        const double dy = -(static_cast<double>(y) + 0.5 - static_cast<double>(H) * 0.5) * pixel_step;
        u = b.center_re + dx * cos_t - dy * sin_t;
        v = b.center_im + dx * sin_t + dy * cos_t;
    } else {
        u = re_min + (static_cast<double>(x) + 0.5) / static_cast<double>(W) * span_re;
        v = im_max - (static_cast<double>(y) + 0.5) / static_cast<double>(H) * span_im;
    }
}

MapStats render_direct_slice_field(const TransitionParams& p, DirectSlice slice, FieldOutput& fo) {
    const bool flip_y = slice == DirectSlice::FromFlipY || slice == DirectSlice::ToFlipY;
    MapParams mp = p.base;
    mp.variant = (slice == DirectSlice::To || slice == DirectSlice::ToFlipY)
        ? p.to_variant : p.from_variant;
    if (flip_y) {
        // theta=-90/+-180 embeds the transition viewport as (u, -v).  A
        // direct 2D render represents that reflection by rendering a mirrored
        // viewport and flipping the rows afterwards.  Reflect every imaginary
        // component and reverse the in-plane view rotation; keeping the
        // original rotation is only equivalent when rotation_deg == 0.
        mp.center_im = -mp.center_im;
        mp.center_im_str = negate_decimal_string(mp.center_im_str);
        mp.julia_im = -mp.julia_im;
        mp.rotation_deg = -mp.rotation_deg;
    }
    MapStats stats = render_map_field(mp, fo);
    if (flip_y) flip_field_output_y(fo);
    return stats;
}

struct TransitionIterResult {
    int    iter;
    double min_abs_sq;
    double max_abs_sq;
    double extra;    // min pairwise distance (sqrt), or 0 if not computed
    double norm;     // |z|² at escape step (0 if not escaped)
    bool   escaped;
    IterResultMask valid_mask;
};

// Direct 3D transition iteration. Tracks min/max of x²+y²+z² for HS metrics.
// Matches cfiles/mandelbrot_3Dtranslation_minmax.c:52.
// When metric == MinPairwiseDist, also stores the orbit and computes min pairwise dist.
struct OrbitPt { double x, y, z; };

constexpr int MAX_TRANSITION_LEGS = 4;

struct ActiveTransitionLeg {
    Variant variant = Variant::Mandelbrot;
    double direction = 1.0;
    double influence = 1.0;
};

struct MultiOrbitPt {
    std::array<double, MAX_TRANSITION_LEGS + 1> coord{};
};

std::vector<ActiveTransitionLeg> active_transition_legs(const std::vector<TransitionLeg>& input) {
    if (input.size() > MAX_TRANSITION_LEGS) {
        throw std::runtime_error("multi transition supports at most 4 variants");
    }

    double max_w = 0.0;
    double sum_w2 = 0.0;
    std::vector<TransitionLeg> kept;
    kept.reserve(input.size());
    for (const TransitionLeg& leg : input) {
        if (!variant_supports_axis_transition(leg.variant)) {
            throw std::runtime_error("transition variants must be quadratic Mandelbrot-family variants");
        }
        if (!std::isfinite(leg.weight)) {
            throw std::runtime_error("invalid transition weight");
        }
        if (leg.weight <= 0.0) continue;
        kept.push_back(leg);
        if (leg.weight > max_w) max_w = leg.weight;
        sum_w2 += leg.weight * leg.weight;
    }
    if (kept.empty() || max_w <= 0.0 || sum_w2 <= 0.0) {
        throw std::runtime_error("multi transition needs at least one positive-weight variant");
    }

    const double inv_len = 1.0 / std::sqrt(sum_w2);
    std::vector<ActiveTransitionLeg> out;
    out.reserve(kept.size());
    for (const TransitionLeg& leg : kept) {
        out.push_back({
            leg.variant,
            leg.weight * inv_len,
            leg.weight / max_w,
        });
    }
    return out;
}

template <Metric M, IterResultMask NeedMask>
inline TransitionIterResult iterate_transition(
    double x0, double y0, double z0,
    int max_iter, double bail2,
    Variant from_variant, Variant to_variant,
    int pairwise_cap,
    std::vector<OrbitPt>& orbit,
    bool julia = false, double jx = 0, double jy = 0, double jz = 0
) {
    const double cx = julia ? jx : x0;
    const double cy = julia ? jy : y0;
    const double cz = julia ? jz : z0;
    double x = x0, y = y0, z = z0;
    double x2 = x * x;
    double y2 = y * y;
    double z2 = z * z;
    TransitionIterResult r{};
    r.iter = 0;
    r.min_abs_sq = std::numeric_limits<double>::infinity();
    r.max_abs_sq = 0.0;
    r.extra = 0.0;
    r.norm = 0.0;
    r.escaped = false;
    r.valid_mask = 0;

    constexpr bool track_iter    = iter_result_wants(NeedMask, IterResultField::Iter);
    constexpr bool track_min_abs = iter_result_wants(NeedMask, IterResultField::MinAbs);
    constexpr bool track_max_abs = iter_result_wants(NeedMask, IterResultField::MaxAbs);
    constexpr bool track_norm    = iter_result_wants(NeedMask, IterResultField::Norm);
    constexpr bool track_escaped = iter_result_wants(NeedMask, IterResultField::Escaped);
    const bool track_orbit = (M == Metric::MinPairwiseDist) && pairwise_cap > 0;

    if constexpr (track_iter)    r.valid_mask |= IterResultField::Iter;
    if constexpr (track_min_abs) r.valid_mask |= IterResultField::MinAbs;
    if constexpr (track_max_abs) r.valid_mask |= IterResultField::MaxAbs;
    if constexpr (track_norm)    r.valid_mask |= IterResultField::Norm;
    if constexpr (track_escaped) r.valid_mask |= IterResultField::Escaped;
    if constexpr (M == Metric::MinPairwiseDist) r.valid_mask |= IterResultField::Extra;

    if constexpr (track_min_abs || track_max_abs) {
        const double init_n2 = x2 + y2 + z2;
        r.min_abs_sq = init_n2;
        r.max_abs_sq = init_n2;
    }

    if (track_orbit) {
        orbit.clear();
        orbit.push_back({x, y, z});
    }

    for (int i = 0; i < max_iter; i++) {
        const double nx =
            variant_transition_real_projection(from_variant, x2, y2)
          + variant_transition_real_projection(to_variant,   x2, z2)
          - x2 + cx;
        const double ny = variant_transition_imag_projection(from_variant, x, y) + cy;
        const double nz = variant_transition_imag_projection(to_variant,   x, z) + cz;
        const bool finite_xyz = std::isfinite(nx) && std::isfinite(ny) && std::isfinite(nz);
        const double nx2 = finite_xyz ? nx * nx : std::numeric_limits<double>::infinity();
        const double ny2 = finite_xyz ? ny * ny : std::numeric_limits<double>::infinity();
        const double nz2 = finite_xyz ? nz * nz : std::numeric_limits<double>::infinity();
        const double n2 = finite_xyz
            ? (nx2 + ny2 + nz2)
            : std::numeric_limits<double>::infinity();
        if constexpr (track_min_abs) {
            if (n2 < r.min_abs_sq) r.min_abs_sq = n2;
        }
        if constexpr (track_max_abs) {
            if (n2 > r.max_abs_sq) r.max_abs_sq = n2;
        }

        if (track_orbit && static_cast<int>(orbit.size()) < pairwise_cap) {
            orbit.push_back({nx, ny, nz});
        }

        if (!finite_xyz || n2 > bail2) {
            if constexpr (track_iter)    r.iter = i;
            if constexpr (track_norm)    r.norm = n2;
            if constexpr (track_escaped) r.escaped = true;
            break;
        }

        x = nx; y = ny; z = nz;
        x2 = nx2; y2 = ny2; z2 = nz2;
    }
    if (!r.escaped) {
        if constexpr (track_iter) r.iter = max_iter;
    }

    // Compute min pairwise distance from orbit (O(n²), capped).
    if (track_orbit && orbit.size() >= 2) {
        double min_d2 = std::numeric_limits<double>::max();
        for (size_t a = 0; a < orbit.size(); a++) {
            for (size_t b = a + 1; b < orbit.size(); b++) {
                const double dx = orbit[a].x - orbit[b].x;
                const double dy = orbit[a].y - orbit[b].y;
                const double dz = orbit[a].z - orbit[b].z;
                const double d2 = dx*dx + dy*dy + dz*dz;
                if (d2 < min_d2) min_d2 = d2;
            }
        }
        r.extra = std::sqrt(min_d2);
    }

    return r;
}

template <Metric M, IterResultMask NeedMask>
inline TransitionIterResult iterate_transition_multi(
    double x0,
    const std::array<double, MAX_TRANSITION_LEGS>& axis0,
    int axis_count,
    const std::vector<ActiveTransitionLeg>& legs,
    int max_iter, double bail2,
    int pairwise_cap,
    std::vector<MultiOrbitPt>& orbit,
    bool julia,
    double jx,
    const std::array<double, MAX_TRANSITION_LEGS>& jaxis
) {
    const double cx = julia ? jx : x0;
    std::array<double, MAX_TRANSITION_LEGS> caxis{};
    for (int k = 0; k < axis_count; ++k) {
        caxis[k] = julia ? jaxis[k] : axis0[k];
    }

    double x = x0;
    double x2 = x * x;
    std::array<double, MAX_TRANSITION_LEGS> axis = axis0;
    std::array<double, MAX_TRANSITION_LEGS> axis2{};
    for (int k = 0; k < axis_count; ++k) axis2[k] = axis[k] * axis[k];

    TransitionIterResult r{};
    r.iter = 0;
    r.min_abs_sq = std::numeric_limits<double>::infinity();
    r.max_abs_sq = 0.0;
    r.extra = 0.0;
    r.norm = 0.0;
    r.escaped = false;
    r.valid_mask = 0;

    constexpr bool track_iter    = iter_result_wants(NeedMask, IterResultField::Iter);
    constexpr bool track_min_abs = iter_result_wants(NeedMask, IterResultField::MinAbs);
    constexpr bool track_max_abs = iter_result_wants(NeedMask, IterResultField::MaxAbs);
    constexpr bool track_norm    = iter_result_wants(NeedMask, IterResultField::Norm);
    constexpr bool track_escaped = iter_result_wants(NeedMask, IterResultField::Escaped);
    const bool track_orbit = (M == Metric::MinPairwiseDist) && pairwise_cap > 0;

    if constexpr (track_iter)    r.valid_mask |= IterResultField::Iter;
    if constexpr (track_min_abs) r.valid_mask |= IterResultField::MinAbs;
    if constexpr (track_max_abs) r.valid_mask |= IterResultField::MaxAbs;
    if constexpr (track_norm)    r.valid_mask |= IterResultField::Norm;
    if constexpr (track_escaped) r.valid_mask |= IterResultField::Escaped;
    if constexpr (M == Metric::MinPairwiseDist) r.valid_mask |= IterResultField::Extra;

    auto norm_sq = [&]() {
        double n2 = x2;
        for (int k = 0; k < axis_count; ++k) n2 += axis2[k];
        return n2;
    };

    if constexpr (track_min_abs || track_max_abs) {
        const double init_n2 = norm_sq();
        r.min_abs_sq = init_n2;
        r.max_abs_sq = init_n2;
    }

    if (track_orbit) {
        orbit.clear();
        MultiOrbitPt p{};
        p.coord[0] = x;
        for (int k = 0; k < axis_count; ++k) p.coord[k + 1] = axis[k];
        orbit.push_back(p);
    }

    std::array<double, MAX_TRANSITION_LEGS> next_axis{};
    for (int i = 0; i < max_iter; ++i) {
        double real_sum = 0.0;
        double influence_sum = 0.0;
        for (int k = 0; k < axis_count; ++k) {
            const double influence = legs[static_cast<size_t>(k)].influence;
            real_sum += influence * variant_transition_real_projection(
                legs[static_cast<size_t>(k)].variant, x2, axis2[k]);
            influence_sum += influence;
            next_axis[k] = influence * variant_transition_imag_projection(
                legs[static_cast<size_t>(k)].variant, x, axis[k]) + caxis[k];
        }

        const double nx = real_sum - (influence_sum - 1.0) * x2 + cx;
        const bool finite_x = std::isfinite(nx);
        const double nx2 = finite_x ? nx * nx : std::numeric_limits<double>::infinity();
        double n2 = finite_x ? nx2 : std::numeric_limits<double>::infinity();
        bool finite_all = finite_x;

        for (int k = 0; k < axis_count; ++k) {
            if (!std::isfinite(next_axis[k])) {
                finite_all = false;
                n2 = std::numeric_limits<double>::infinity();
                break;
            }
            const double a2 = next_axis[k] * next_axis[k];
            n2 += a2;
        }

        if constexpr (track_min_abs) {
            if (n2 < r.min_abs_sq) r.min_abs_sq = n2;
        }
        if constexpr (track_max_abs) {
            if (n2 > r.max_abs_sq) r.max_abs_sq = n2;
        }

        if (track_orbit && static_cast<int>(orbit.size()) < pairwise_cap) {
            MultiOrbitPt p{};
            p.coord[0] = nx;
            for (int k = 0; k < axis_count; ++k) p.coord[k + 1] = next_axis[k];
            orbit.push_back(p);
        }

        if (!finite_all || n2 > bail2) {
            if constexpr (track_iter)    r.iter = i;
            if constexpr (track_norm)    r.norm = n2;
            if constexpr (track_escaped) r.escaped = true;
            break;
        }

        x = nx;
        x2 = nx2;
        for (int k = 0; k < axis_count; ++k) {
            axis[k] = next_axis[k];
            axis2[k] = axis[k] * axis[k];
        }
    }
    if (!r.escaped) {
        if constexpr (track_iter) r.iter = max_iter;
    }

    if (track_orbit && orbit.size() >= 2) {
        double min_d2 = std::numeric_limits<double>::max();
        for (size_t a = 0; a < orbit.size(); ++a) {
            for (size_t b = a + 1; b < orbit.size(); ++b) {
                double d2 = 0.0;
                for (int k = 0; k <= axis_count; ++k) {
                    const double d = orbit[a].coord[k] - orbit[b].coord[k];
                    d2 += d * d;
                }
                if (d2 < min_d2) min_d2 = d2;
            }
        }
        r.extra = std::sqrt(min_d2);
    }

    return r;
}

template <Metric M>
double transition_raw_value(const TransitionIterResult& r) {
    if constexpr (M == Metric::MinAbs) {
        return std::sqrt(r.min_abs_sq);
    } else if constexpr (M == Metric::MaxAbs) {
        return std::sqrt(r.max_abs_sq);
    } else if constexpr (M == Metric::Envelope) {
        return 0.5 * (std::sqrt(r.min_abs_sq) + std::sqrt(r.max_abs_sq));
    } else if constexpr (M == Metric::MinPairwiseDist) {
        return r.extra;
    } else {
        return 0.0;
    }
}

template <Metric M>
double transition_normalized_value(const TransitionIterResult& r, double bailout) {
    if constexpr (M == Metric::MinAbs) {
        return std::min(1.0, std::sqrt(r.min_abs_sq) / bailout);
    } else if constexpr (M == Metric::MaxAbs) {
        return std::min(1.0, std::sqrt(r.max_abs_sq) / bailout);
    } else if constexpr (M == Metric::Envelope) {
        return std::min(1.0, 0.5 * (std::sqrt(r.min_abs_sq) + std::sqrt(r.max_abs_sq)) / bailout);
    } else if constexpr (M == Metric::MinPairwiseDist) {
        return std::min(1.0, r.extra / bailout);
    } else {
        return 0.0;
    }
}

template <Metric M, IterResultMask NeedMask>
MapStats render_transition_metric_field(const TransitionParams& p, FieldOutput& fo) {
    if (!variant_supports_axis_transition(p.from_variant) ||
        !variant_supports_axis_transition(p.to_variant)) {
        throw std::runtime_error("transition variants must be quadratic Mandelbrot-family variants");
    }

    const auto t0 = std::chrono::steady_clock::now();
    const auto& b = p.base;

    const int W = b.width;
    const int H = b.height;
    const size_t npx = static_cast<size_t>(W) * H;
    fo.width = W;
    fo.height = H;
    fo.metric = M;
    if constexpr (M == Metric::Escape) {
        fo.iter_u32.resize(npx);
        fo.norm_f32.resize(npx);
    } else {
        fo.field_f64.resize(npx);
    }

    const double aspect  = static_cast<double>(W) / H;
    const double span_im = b.scale;
    const double span_re = b.scale * aspect;
    const double re_min  = b.center_re - span_re * 0.5;
    const double im_max  = b.center_im + span_im * 0.5;
    const double bail2   = b.bailout_sq;
    const double cth     = std::cos(p.theta);
    const double sth     = std::sin(p.theta);
    const bool has_view_rot = b.rotation_deg != 0.0;
    const double view_rad = has_view_rot ? b.rotation_deg * PI / 180.0 : 0.0;
    const double view_cos = has_view_rot ? std::cos(view_rad) : 1.0;
    const double view_sin = has_view_rot ? std::sin(view_rad) : 0.0;
    const int thread_count = resolve_render_threads(b.render_threads);
    constexpr int tile_size = 32;
    const int tiles_x = (W + tile_size - 1) / tile_size;
    const int tiles_y = (H + tile_size - 1) / tile_size;
    const int tile_count = tiles_x * tiles_y;
    std::atomic<bool> cancelled{false};

    double global_min = std::numeric_limits<double>::infinity();
    double global_max = -std::numeric_limits<double>::infinity();

    #pragma omp parallel num_threads(thread_count)
    {
        std::vector<OrbitPt> orbit;
        if constexpr (M == Metric::MinPairwiseDist) {
            orbit.reserve(static_cast<size_t>(b.pairwise_cap));
        }
        double local_min = std::numeric_limits<double>::infinity();
        double local_max = -std::numeric_limits<double>::infinity();

    #pragma omp for schedule(dynamic, 1)
    for (int tile = 0; tile < tile_count; tile++) {
        if (mark_cancelled_if_requested(p, cancelled)) continue;
        const int tile_x = tile % tiles_x;
        const int tile_y = tile / tiles_x;
        const int x_begin = tile_x * tile_size;
        const int y_begin = tile_y * tile_size;
        const int x_end = std::min(W, x_begin + tile_size);
        const int y_end = std::min(H, y_begin + tile_size);

        for (int y = y_begin; y < y_end; y++) {
            if (mark_cancelled_if_requested(p, cancelled)) break;
            const size_t row_off = static_cast<size_t>(y) * W;
            for (int x = x_begin; x < x_end; x++) {
                double u = 0.0;
                double v = 0.0;
                transition_viewport_point(b, W, H, x, y,
                                          re_min, im_max, span_re, span_im,
                                          has_view_rot, view_cos, view_sin,
                                          u, v);

                const TransitionIterResult r =
                    iterate_transition<M, NeedMask>(u, v * cth, v * sth,
                                       b.iterations, bail2,
                                       p.from_variant, p.to_variant,
                                       b.pairwise_cap, orbit,
                                       b.julia, b.julia_re,
                                       b.julia_im * cth, b.julia_im * sth);

                const size_t idx = row_off + static_cast<size_t>(x);
                if constexpr (M == Metric::Escape) {
                    fo.iter_u32[idx] = static_cast<uint32_t>(
                        r.escaped ? r.iter : b.iterations);
                    constexpr bool has_norm =
                        iter_result_wants(NeedMask, IterResultField::Norm);
                    fo.norm_f32[idx] = has_norm && r.escaped
                        ? static_cast<float>(r.norm) : 0.0f;
                } else {
                    const double fv = transition_raw_value<M>(r);
                    fo.field_f64[idx] = fv;
                    if (fv < local_min) local_min = fv;
                    if (fv > local_max) local_max = fv;
                }
            }
        }
    }

    if constexpr (M != Metric::Escape) {
        #pragma omp critical
        {
            if (local_min < global_min) global_min = local_min;
            if (local_max > global_max) global_max = local_max;
        }
    }
    } // end omp parallel

    if (cancelled.load(std::memory_order_relaxed) || transition_cancel_requested(p)) {
        throw_transition_cancelled();
    }

    if constexpr (M != Metric::Escape) {
        fo.field_min = global_min;
        fo.field_max = global_max;
    }

    const auto t1 = std::chrono::steady_clock::now();
    MapStats s;
    s.elapsed_ms  = std::chrono::duration<double, std::milli>(t1 - t0).count();
    s.pixel_count = W * H;
    s.scalar_used = "fp64";
    s.engine_used = "openmp";
    fo.scalar_used = s.scalar_used;
    fo.engine_used = s.engine_used;
    fo.elapsed_ms  = s.elapsed_ms;
    return s;
}

template <Metric M, IterResultMask NeedMask>
MapStats render_transition_multi_field(const TransitionParams& p, FieldOutput& fo) {
    const std::vector<ActiveTransitionLeg> legs = active_transition_legs(p.multi_legs);
    const int axis_count = static_cast<int>(legs.size());

    const auto t0 = std::chrono::steady_clock::now();
    const auto& b = p.base;

    const int W = b.width;
    const int H = b.height;
    const size_t npx = static_cast<size_t>(W) * H;
    fo.width = W;
    fo.height = H;
    fo.metric = M;
    if constexpr (M == Metric::Escape) {
        fo.iter_u32.resize(npx);
        fo.norm_f32.resize(npx);
    } else {
        fo.field_f64.resize(npx);
    }

    const double aspect  = static_cast<double>(W) / H;
    const double span_im = b.scale;
    const double span_re = b.scale * aspect;
    const double re_min  = b.center_re - span_re * 0.5;
    const double im_max  = b.center_im + span_im * 0.5;
    const double bail2   = b.bailout_sq;
    const bool has_view_rot = b.rotation_deg != 0.0;
    const double view_rad = has_view_rot ? b.rotation_deg * PI / 180.0 : 0.0;
    const double view_cos = has_view_rot ? std::cos(view_rad) : 1.0;
    const double view_sin = has_view_rot ? std::sin(view_rad) : 0.0;
    const int thread_count = resolve_render_threads(b.render_threads);
    constexpr int tile_size = 32;
    const int tiles_x = (W + tile_size - 1) / tile_size;
    const int tiles_y = (H + tile_size - 1) / tile_size;
    const int tile_count = tiles_x * tiles_y;
    std::atomic<bool> cancelled{false};

    double global_min = std::numeric_limits<double>::infinity();
    double global_max = -std::numeric_limits<double>::infinity();

    #pragma omp parallel num_threads(thread_count)
    {
        std::vector<MultiOrbitPt> orbit;
        if constexpr (M == Metric::MinPairwiseDist) {
            orbit.reserve(static_cast<size_t>(b.pairwise_cap));
        }
        double local_min = std::numeric_limits<double>::infinity();
        double local_max = -std::numeric_limits<double>::infinity();

    #pragma omp for schedule(dynamic, 1)
    for (int tile = 0; tile < tile_count; tile++) {
        if (mark_cancelled_if_requested(p, cancelled)) continue;
        const int tile_x = tile % tiles_x;
        const int tile_y = tile / tiles_x;
        const int x_begin = tile_x * tile_size;
        const int y_begin = tile_y * tile_size;
        const int x_end = std::min(W, x_begin + tile_size);
        const int y_end = std::min(H, y_begin + tile_size);

        for (int y = y_begin; y < y_end; y++) {
            if (mark_cancelled_if_requested(p, cancelled)) break;
            const size_t row_off = static_cast<size_t>(y) * W;
            for (int x = x_begin; x < x_end; x++) {
                double u = 0.0;
                double v = 0.0;
                transition_viewport_point(b, W, H, x, y,
                                          re_min, im_max, span_re, span_im,
                                          has_view_rot, view_cos, view_sin,
                                          u, v);

                std::array<double, MAX_TRANSITION_LEGS> axis0{};
                std::array<double, MAX_TRANSITION_LEGS> jaxis{};
                for (int k = 0; k < axis_count; ++k) {
                    axis0[k] = v * legs[static_cast<size_t>(k)].direction;
                    jaxis[k] = b.julia_im * legs[static_cast<size_t>(k)].direction;
                }

                const TransitionIterResult r =
                    iterate_transition_multi<M, NeedMask>(
                        u, axis0, axis_count, legs,
                        b.iterations, bail2,
                        b.pairwise_cap, orbit,
                        b.julia, b.julia_re, jaxis);

                const size_t idx = row_off + static_cast<size_t>(x);
                if constexpr (M == Metric::Escape) {
                    fo.iter_u32[idx] = static_cast<uint32_t>(
                        r.escaped ? r.iter : b.iterations);
                    constexpr bool has_norm =
                        iter_result_wants(NeedMask, IterResultField::Norm);
                    fo.norm_f32[idx] = has_norm && r.escaped
                        ? static_cast<float>(r.norm) : 0.0f;
                } else {
                    const double fv = transition_raw_value<M>(r);
                    fo.field_f64[idx] = fv;
                    if (fv < local_min) local_min = fv;
                    if (fv > local_max) local_max = fv;
                }
            }
        }
    }

    if constexpr (M != Metric::Escape) {
        #pragma omp critical
        {
            if (local_min < global_min) global_min = local_min;
            if (local_max > global_max) global_max = local_max;
        }
    }
    } // end omp parallel

    if (cancelled.load(std::memory_order_relaxed) || transition_cancel_requested(p)) {
        throw_transition_cancelled();
    }

    if constexpr (M != Metric::Escape) {
        fo.field_min = global_min;
        fo.field_max = global_max;
    }

    const auto t1 = std::chrono::steady_clock::now();
    MapStats s;
    s.elapsed_ms  = std::chrono::duration<double, std::milli>(t1 - t0).count();
    s.pixel_count = W * H;
    s.scalar_used = "fp64";
    s.engine_used = "openmp_multi";
    fo.scalar_used = s.scalar_used;
    fo.engine_used = s.engine_used;
    fo.elapsed_ms  = s.elapsed_ms;
    return s;
}

// ── CUDA bridge ─────────────────────────────────────────────────────────────

#if defined(HAS_CUDA_KERNEL)
MapStats render_transition_cuda(const TransitionParams& p, FieldOutput& fo) {
    const auto& b = p.base;
    const int W = b.width, H = b.height;
    const size_t npx = static_cast<size_t>(W) * H;

    fsd_cuda::CudaTransitionSliceParams cp;
    cp.center_re  = b.center_re;
    cp.center_im  = b.center_im;
    cp.scale      = b.scale;
    const double rotation_rad = b.rotation_deg * PI / 180.0;
    cp.cos_rotation = std::cos(rotation_rad);
    cp.sin_rotation = std::sin(rotation_rad);
    cp.width      = W;
    cp.height     = H;
    cp.iterations = b.iterations;
    cp.bailout_sq = b.bailout_sq;
    cp.cos_theta  = std::cos(p.theta);
    cp.sin_theta  = std::sin(p.theta);
    cp.from_variant = static_cast<int>(p.from_variant);
    cp.to_variant   = static_cast<int>(p.to_variant);
    if (!p.multi_legs.empty()) {
        const std::vector<ActiveTransitionLeg> legs = active_transition_legs(p.multi_legs);
        cp.multi_count = static_cast<int>(legs.size());
        for (int i = 0; i < cp.multi_count; ++i) {
            cp.multi_variants[i] = static_cast<int>(legs[static_cast<size_t>(i)].variant);
            cp.multi_direction[i] = legs[static_cast<size_t>(i)].direction;
            cp.multi_influence[i] = legs[static_cast<size_t>(i)].influence;
        }
    }
    cp.julia     = b.julia;
    cp.julia_re  = b.julia_re;
    cp.julia_im  = b.julia_im;

    const std::string eff = map_effective_scalar_type(b);
    cp.scalar_type = map_scalar_type_is_fp32(eff) ? "fp32" : "fp64";

    fo.width  = W;
    fo.height = H;

    MapStats s;

    if (b.metric == Metric::Escape) {
        cp.metric_id = 0;
        fo.metric = Metric::Escape;
        fo.iter_u32.resize(npx);
        fo.norm_f32.resize(npx);
        auto cs = fsd_cuda::cuda_render_transition_slice_escape(
            cp, fo.iter_u32.data(), fo.norm_f32.data());
        s.elapsed_ms  = cs.elapsed_ms;
        s.scalar_used = cs.scalar_used;
        s.engine_used = cs.engine_used;
    } else {
        if (b.metric == Metric::MinAbs) cp.metric_id = 1;
        else if (b.metric == Metric::MaxAbs) cp.metric_id = 2;
        else cp.metric_id = 3;
        fo.metric = b.metric;
        std::vector<float> field_f32(npx);
        float fmin = 0, fmax = 0;
        auto cs = fsd_cuda::cuda_render_transition_slice_metric(
            cp, field_f32.data(), fmin, fmax);
        fo.field_f64.resize(npx);
        for (size_t i = 0; i < npx; i++)
            fo.field_f64[i] = static_cast<double>(field_f32[i]);
        fo.field_min = static_cast<double>(fmin);
        fo.field_max = static_cast<double>(fmax);
        s.elapsed_ms  = cs.elapsed_ms;
        s.scalar_used = cs.scalar_used;
        s.engine_used = cs.engine_used;
    }

    s.pixel_count  = W * H;
    fo.scalar_used = s.scalar_used;
    fo.engine_used = s.engine_used;
    fo.elapsed_ms  = s.elapsed_ms;
    return s;
}
#endif

// ── High-precision scalar path ──────────────────────────────────────────────

template <typename S>
inline S transition_real_projection_s(Variant v, S x2, S axis2) {
    S q = x2 - axis2;
    return variant_transition_post_abs_real(v) ? scalar_abs(q) : q;
}

template <typename S>
inline S transition_imag_projection_s(Variant v, S x, S axis) {
    switch (v) {
        case Variant::Tri:
        case Variant::Vase:
            return scalar_from_double<S>(-2.0) * x * axis;
        case Variant::Boat:
        case Variant::Bird:
            return scalar_from_double<S>(2.0) * scalar_abs(x * axis);
        case Variant::Duck:
        case Variant::Mask:
            return scalar_from_double<S>(2.0) * x * scalar_abs(axis);
        case Variant::Bell:
        case Variant::Ship:
            return scalar_from_double<S>(-2.0) * scalar_abs(x) * axis;
        case Variant::Mandelbrot:
        case Variant::Fish:
        default:
            return scalar_from_double<S>(2.0) * x * axis;
    }
}

template <typename S, Metric M, IterResultMask NeedMask>
MapStats render_transition_scalar(const TransitionParams& p, FieldOutput& fo) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto& b = p.base;
    const int W = b.width, H = b.height;
    const size_t npx = static_cast<size_t>(W) * H;

    fo.width = W; fo.height = H; fo.metric = M;
    if constexpr (M == Metric::Escape) {
        fo.iter_u32.resize(npx);
        fo.norm_f32.resize(npx);
    } else {
        fo.field_f64.resize(npx);
    }

    const S s_center_re = scalar_from_string<S>(b.center_re_str, b.center_re);
    const S s_center_im = scalar_from_string<S>(b.center_im_str, b.center_im);
    const S s_scale     = scalar_from_double<S>(b.scale);
    const S s_aspect    = scalar_from_double<S>(static_cast<double>(W) / H);
    const S s_span_im   = s_scale;
    const S s_span_re   = s_scale * s_aspect;
    const S s_half      = scalar_from_double<S>(0.5);
    const S s_re_min    = s_center_re - s_span_re * s_half;
    const S s_im_max    = s_center_im + s_span_im * s_half;
    const S s_inv_W     = scalar_from_double<S>(1.0 / W);
    const S s_inv_H     = scalar_from_double<S>(1.0 / H);
    const bool has_view_rot = b.rotation_deg != 0.0;
    const double view_rad = has_view_rot ? b.rotation_deg * PI / 180.0 : 0.0;
    const S s_view_cos = scalar_from_double<S>(has_view_rot ? std::cos(view_rad) : 1.0);
    const S s_view_sin = scalar_from_double<S>(has_view_rot ? std::sin(view_rad) : 0.0);
    const S s_pixel_step = s_scale / scalar_from_double<S>(static_cast<double>(H));
    const S s_half_w = scalar_from_double<S>(static_cast<double>(W) * 0.5);
    const S s_half_h = scalar_from_double<S>(static_cast<double>(H) * 0.5);
    const double bail2  = b.bailout_sq;

    const int thread_count = resolve_render_threads(b.render_threads);
    std::atomic<bool> cancelled{false};

    double global_min =  std::numeric_limits<double>::infinity();
    double global_max = -std::numeric_limits<double>::infinity();

    #pragma omp parallel num_threads(thread_count)
    {
    double local_min =  std::numeric_limits<double>::infinity();
    double local_max = -std::numeric_limits<double>::infinity();

    #pragma omp for schedule(dynamic, 4)
    for (int y = 0; y < H; y++) {
        if (mark_cancelled_if_requested(p, cancelled)) continue;
        const S s_v_norot = s_im_max - scalar_from_double<S>(static_cast<double>(y) + 0.5) * s_inv_H * s_span_im;
        const S s_dy = -(scalar_from_double<S>(static_cast<double>(y) + 0.5) - s_half_h) * s_pixel_step;
        const size_t row_off = static_cast<size_t>(y) * W;

        for (int x = 0; x < W; x++) {
            S s_u;
            S s_v;
            if (has_view_rot) {
                const S s_dx = (scalar_from_double<S>(static_cast<double>(x) + 0.5) - s_half_w) * s_pixel_step;
                s_u = s_center_re + s_dx * s_view_cos - s_dy * s_view_sin;
                s_v = s_center_im + s_dx * s_view_sin + s_dy * s_view_cos;
            } else {
                s_u = s_re_min + scalar_from_double<S>(static_cast<double>(x) + 0.5) * s_inv_W * s_span_re;
                s_v = s_v_norot;
            }

            const double u  = static_cast<double>(s_u);
            const double v  = static_cast<double>(s_v);
            const double x0 = u;
            const double y0 = v * std::cos(p.theta);
            const double z0 = v * std::sin(p.theta);
            const double cx = b.julia ? b.julia_re                         : x0;
            const double cy = b.julia ? b.julia_im * std::cos(p.theta)     : y0;
            const double cz = b.julia ? b.julia_im * std::sin(p.theta)     : z0;

            double xv = x0, yv = y0, zv = z0;
            double xv2 = xv*xv, yv2 = yv*yv, zv2 = zv*zv;
            double min_sq = xv2 + yv2 + zv2, max_sq = min_sq;
            int iter = 0;
            bool escaped = false;
            double norm = 0.0;

            for (; iter < b.iterations; iter++) {
                const double nx = variant_transition_real_projection(p.from_variant, xv2, yv2)
                                + variant_transition_real_projection(p.to_variant,   xv2, zv2)
                                - xv2 + cx;
                const double ny = variant_transition_imag_projection(p.from_variant, xv, yv) + cy;
                const double nz = variant_transition_imag_projection(p.to_variant,   xv, zv) + cz;
                const bool fin = std::isfinite(nx) && std::isfinite(ny) && std::isfinite(nz);
                const double n2 = fin ? (nx*nx + ny*ny + nz*nz) : std::numeric_limits<double>::infinity();
                if constexpr (iter_result_wants(NeedMask, IterResultField::MinAbs)) {
                    if (n2 < min_sq) min_sq = n2;
                }
                if constexpr (iter_result_wants(NeedMask, IterResultField::MaxAbs)) {
                    if (n2 > max_sq) max_sq = n2;
                }
                if (!fin || n2 > bail2) { norm = n2; escaped = true; break; }
                xv = nx; yv = ny; zv = nz;
                xv2 = nx*nx; yv2 = ny*ny; zv2 = nz*nz;
            }

            const size_t idx = row_off + static_cast<size_t>(x);
            if constexpr (M == Metric::Escape) {
                fo.iter_u32[idx] = static_cast<uint32_t>(escaped ? iter : b.iterations);
                fo.norm_f32[idx] = escaped ? static_cast<float>(norm) : 0.0f;
            } else {
                double fv = 0.0;
                if constexpr (M == Metric::MinAbs)  fv = std::sqrt(min_sq);
                if constexpr (M == Metric::MaxAbs)   fv = std::sqrt(max_sq);
                if constexpr (M == Metric::Envelope) fv = 0.5 * (std::sqrt(min_sq) + std::sqrt(max_sq));
                fo.field_f64[idx] = fv;
                if (fv < local_min) local_min = fv;
                if (fv > local_max) local_max = fv;
            }
        }
    }

    if constexpr (M != Metric::Escape) {
        #pragma omp critical
        {
            if (local_min < global_min) global_min = local_min;
            if (local_max > global_max) global_max = local_max;
        }
    }
    } // end omp parallel

    if (cancelled.load(std::memory_order_relaxed) || transition_cancel_requested(p))
        throw_transition_cancelled();

    if constexpr (M != Metric::Escape) {
        fo.field_min = global_min;
        fo.field_max = global_max;
    }

    const auto t1 = std::chrono::steady_clock::now();
    const char* sname = std::is_same_v<S, long double> ? "fp80" : "fp128";
    MapStats s;
    s.elapsed_ms  = std::chrono::duration<double, std::milli>(t1 - t0).count();
    s.pixel_count = W * H;
    s.scalar_used = sname;
    s.engine_used = "openmp";
    fo.scalar_used = s.scalar_used;
    fo.engine_used = s.engine_used;
    fo.elapsed_ms  = s.elapsed_ms;
    return s;
}

template <typename S>
MapStats dispatch_transition_scalar(const TransitionParams& q, FieldOutput& fo) {
    switch (q.base.metric) {
        case Metric::Escape:
            return render_transition_scalar<S, Metric::Escape,
                IterResultField::Iter | IterResultField::Escaped | IterResultField::Norm>(q, fo);
        case Metric::MinAbs:
            return render_transition_scalar<S, Metric::MinAbs, IterResultField::MinAbs>(q, fo);
        case Metric::MaxAbs:
            return render_transition_scalar<S, Metric::MaxAbs, IterResultField::MaxAbs>(q, fo);
        case Metric::Envelope:
            return render_transition_scalar<S, Metric::Envelope,
                IterResultField::MinAbs | IterResultField::MaxAbs>(q, fo);
        default:
            return render_transition_scalar<S, Metric::Escape,
                IterResultField::Iter | IterResultField::Escaped | IterResultField::Norm>(q, fo);
    }
}

// ── OpenMP fp64 dispatch ────────────────────────────────────────────────────

MapStats dispatch_transition_openmp(const TransitionParams& q, FieldOutput& fo) {
    switch (q.base.metric) {
        case Metric::Escape:
            return render_transition_metric_field<Metric::Escape,
                IterResultField::Iter | IterResultField::Escaped | IterResultField::Norm>(q, fo);
        case Metric::MinAbs:
            return render_transition_metric_field<Metric::MinAbs, IterResultField::MinAbs>(q, fo);
        case Metric::MaxAbs:
            return render_transition_metric_field<Metric::MaxAbs, IterResultField::MaxAbs>(q, fo);
        case Metric::Envelope:
            return render_transition_metric_field<Metric::Envelope,
                IterResultField::MinAbs | IterResultField::MaxAbs>(q, fo);
        case Metric::MinPairwiseDist:
            return render_transition_metric_field<Metric::MinPairwiseDist, IterResultField::Extra>(q, fo);
        default:
            break;
    }
    return render_transition_metric_field<Metric::Escape,
        IterResultField::Iter | IterResultField::Escaped | IterResultField::Norm>(q, fo);
}

MapStats dispatch_transition_multi_openmp(const TransitionParams& q, FieldOutput& fo) {
    switch (q.base.metric) {
        case Metric::Escape:
            return render_transition_multi_field<Metric::Escape,
                IterResultField::Iter | IterResultField::Escaped | IterResultField::Norm>(q, fo);
        case Metric::MinAbs:
            return render_transition_multi_field<Metric::MinAbs, IterResultField::MinAbs>(q, fo);
        case Metric::MaxAbs:
            return render_transition_multi_field<Metric::MaxAbs, IterResultField::MaxAbs>(q, fo);
        case Metric::Envelope:
            return render_transition_multi_field<Metric::Envelope,
                IterResultField::MinAbs | IterResultField::MaxAbs>(q, fo);
        case Metric::MinPairwiseDist:
            return render_transition_multi_field<Metric::MinPairwiseDist, IterResultField::Extra>(q, fo);
        default:
            break;
    }
    return render_transition_multi_field<Metric::Escape,
        IterResultField::Iter | IterResultField::Escaped | IterResultField::Norm>(q, fo);
}

} // namespace

MapStats render_transition_field(const TransitionParams& p, FieldOutput& fo) {
    if (p.multi_legs.empty() && (!variant_supports_axis_transition(p.from_variant) ||
        !variant_supports_axis_transition(p.to_variant))) {
        throw std::runtime_error("transition variants must be quadratic Mandelbrot-family variants");
    }
    if (transition_cancel_requested(p)) throw_transition_cancelled();

    const NormalizedTheta theta = normalize_transition_theta(p);
    const DirectSlice direct = direct_slice_for_milli_deg(theta.milli_deg);
    if (p.multi_legs.empty() && direct != DirectSlice::None) {
        return render_direct_slice_field(p, direct, fo);
    }

    TransitionParams q = p;
    q.theta = theta.radians;

    // ── Engine dispatch ─────────────────────────────────────────────────────
    const std::string eff = map_effective_scalar_type(q.base);
    const bool need_hp = map_scalar_type_is_fp128(eff) || map_scalar_type_is_fp80(eff);
    const bool pairwise = (q.base.metric == Metric::MinPairwiseDist);

    // High-precision scalar path (fp128 / fp80)
    if (need_hp && !pairwise && q.multi_legs.empty()) {
#if defined(FSD_HAS_FLOAT128)
        if (map_scalar_type_is_fp128(eff))
            return dispatch_transition_scalar<__float128>(q, fo);
#endif
        return dispatch_transition_scalar<long double>(q, fo);
    }

    // MinPairwiseDist requires orbit storage — OpenMP only
    if (pairwise) {
        return q.multi_legs.empty()
            ? dispatch_transition_openmp(q, fo)
            : dispatch_transition_multi_openmp(q, fo);
    }

    // Try CUDA
    const std::string engine = q.base.engine;
#if defined(HAS_CUDA_KERNEL)
    if (engine == "auto" || engine == "cuda") {
        if (fsd_cuda::cuda_transition_slice_available()) {
            return render_transition_cuda(q, fo);
        }
    }
#endif

    // Try AVX2
    if (engine == "auto" || engine == "avx2") {
        if (render_transition_field_avx2(q, fo)) {
            return MapStats{fo.elapsed_ms, q.base.width * q.base.height,
                            fo.scalar_used, fo.engine_used};
        }
    }

    // Fallback: OpenMP fp64
    return q.multi_legs.empty()
        ? dispatch_transition_openmp(q, fo)
        : dispatch_transition_multi_openmp(q, fo);
}

MapStats render_transition(const TransitionParams& p, cv::Mat& out) {
    FieldOutput fo;
    auto stats = render_transition_field(p, fo);
    out = colorize_direct(p.base, fo);
    return stats;
}

} // namespace fsd::compute
