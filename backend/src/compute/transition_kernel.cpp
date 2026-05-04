// compute/transition_kernel.cpp

#include "transition_kernel.hpp"
#include "map_kernel.hpp"
#include "parallel.hpp"

#ifdef _OPENMP
#  include <omp.h>
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
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
    return p.should_cancel && p.should_cancel();
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

MapStats render_direct_slice(const TransitionParams& p, DirectSlice slice, cv::Mat& out) {
    const bool flip_y = slice == DirectSlice::FromFlipY || slice == DirectSlice::ToFlipY;

    MapParams mp;
    mp.center_re = p.center_re;
    mp.center_im = flip_y ? -p.center_im : p.center_im;
    mp.scale = p.scale;
    mp.width = p.width;
    mp.height = p.height;
    mp.iterations = p.iterations;
    mp.bailout = p.bailout;
    mp.bailout_sq = p.bailout_sq;
    mp.variant = (slice == DirectSlice::To || slice == DirectSlice::ToFlipY)
        ? p.to_variant
        : p.from_variant;
    mp.metric = p.metric;
    mp.colormap = p.colormap;
    mp.smooth = p.smooth;
    mp.pairwise_cap = p.pairwise_cap;
    mp.render_threads = p.render_threads;
    mp.scalar_type = p.scalar_type;
    mp.engine = p.engine;
    mp.should_cancel = p.should_cancel;

    MapStats stats = render_map(mp, out);
    if (flip_y) cv::flip(out, out, 0);
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

template <Metric M, IterResultMask NeedMask>
inline TransitionIterResult iterate_transition(
    double x0, double y0, double z0,
    int max_iter, double bail2,
    Variant from_variant, Variant to_variant,
    int pairwise_cap,
    std::vector<OrbitPt>& orbit
) {
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
          - x2 + x0;
        const double ny = variant_transition_imag_projection(from_variant, x, y) + y0;
        const double nz = variant_transition_imag_projection(to_variant,   x, z) + z0;
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
MapStats render_transition_metric(const TransitionParams& p, cv::Mat& out) {
    if (!variant_supports_axis_transition(p.from_variant) ||
        !variant_supports_axis_transition(p.to_variant)) {
        throw std::runtime_error("transition variants must be quadratic Mandelbrot-family variants");
    }

    if (out.empty() || out.rows != p.height || out.cols != p.width || out.type() != CV_8UC3) {
        out.create(p.height, p.width, CV_8UC3);
    }

    const auto t0 = std::chrono::steady_clock::now();

    const int W = p.width;
    const int H = p.height;
    const double aspect  = static_cast<double>(W) / H;
    const double span_im = p.scale;
    const double span_re = p.scale * aspect;
    const double re_min  = p.center_re - span_re * 0.5;
    const double im_max  = p.center_im + span_im * 0.5;
    const double bail2   = p.bailout_sq;
    const double cth     = std::cos(p.theta);
    const double sth     = std::sin(p.theta);
    const int thread_count = resolve_render_threads(p.render_threads);
    constexpr int tile_size = 32;
    const int tiles_x = (W + tile_size - 1) / tile_size;
    const int tiles_y = (H + tile_size - 1) / tile_size;
    const int tile_count = tiles_x * tiles_y;
    std::atomic<bool> cancelled{false};

    #pragma omp parallel num_threads(thread_count)
    {
        std::vector<OrbitPt> orbit;
        if constexpr (M == Metric::MinPairwiseDist) {
            orbit.reserve(static_cast<size_t>(p.pairwise_cap));
        }

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
            uint8_t* row = out.ptr<uint8_t>(y);
            const double v = im_max - (static_cast<double>(y) + 0.5) / H * span_im;
            for (int x = x_begin; x < x_end; x++) {
                const double u = re_min + (static_cast<double>(x) + 0.5) / W * span_re;

                const double x0 = u;
                const double y0 = v * cth;
                const double z0 = v * sth;

                const TransitionIterResult r =
                    iterate_transition<M, NeedMask>(x0, y0, z0, p.iterations, bail2,
                                       p.from_variant, p.to_variant,
                                       p.pairwise_cap, orbit);

                uint8_t* px = row + 3 * x;
                if constexpr (M == Metric::Escape) {
                    const int iter = r.escaped ? r.iter : p.iterations;
                    constexpr bool smooth_escape = iter_result_wants(NeedMask, IterResultField::Norm);
                    const double norm = smooth_escape && r.escaped ? r.norm : 0.0;
                    colorize_escape_bgr(iter, p.iterations, p.colormap, norm, smooth_escape, px[0], px[1], px[2]);
                } else {
                    if (p.colormap == Colormap::HsRainbow) {
                        const double fv = transition_raw_value<M>(r);
                        colorize_field_hs_bgr(fv, px[0], px[1], px[2]);
                    } else if (p.smooth) {
                        const double fv = transition_raw_value<M>(r);
                        colorize_field_smooth_bgr(fv, p.colormap, px[0], px[1], px[2]);
                    } else {
                        const double v01 = transition_normalized_value<M>(r, p.bailout);
                        colorize_field_bgr(v01, p.colormap, px[0], px[1], px[2]);
                    }
                }
            }
        }
    }
    } // end omp parallel
    if (cancelled.load(std::memory_order_relaxed) || transition_cancel_requested(p)) {
        throw_transition_cancelled();
    }

    const auto t1 = std::chrono::steady_clock::now();
    MapStats s;
    s.elapsed_ms  = std::chrono::duration<double, std::milli>(t1 - t0).count();
    s.pixel_count = p.width * p.height;
    s.scalar_used = "fp64";
    s.engine_used = "openmp";
    return s;
}

} // namespace

MapStats render_transition(const TransitionParams& p, cv::Mat& out) {
    if (!variant_supports_axis_transition(p.from_variant) ||
        !variant_supports_axis_transition(p.to_variant)) {
        throw std::runtime_error("transition variants must be quadratic Mandelbrot-family variants");
    }
    if (transition_cancel_requested(p)) throw_transition_cancelled();

    const NormalizedTheta theta = normalize_transition_theta(p);
    const DirectSlice direct = direct_slice_for_milli_deg(theta.milli_deg);
    if (direct != DirectSlice::None) {
        return render_direct_slice(p, direct, out);
    }

    TransitionParams q = p;
    q.theta = theta.radians;
    switch (p.metric) {
        case Metric::Escape:
            if (p.smooth) {
                return render_transition_metric<Metric::Escape,
                    IterResultField::Iter | IterResultField::Escaped | IterResultField::Norm>(q, out);
            }
            return render_transition_metric<Metric::Escape,
                IterResultField::Iter | IterResultField::Escaped>(q, out);
        case Metric::MinAbs:
            return render_transition_metric<Metric::MinAbs, IterResultField::MinAbs>(q, out);
        case Metric::MaxAbs:
            return render_transition_metric<Metric::MaxAbs, IterResultField::MaxAbs>(q, out);
        case Metric::Envelope:
            return render_transition_metric<Metric::Envelope,
                IterResultField::MinAbs | IterResultField::MaxAbs>(q, out);
        case Metric::MinPairwiseDist:
            return render_transition_metric<Metric::MinPairwiseDist, IterResultField::Extra>(q, out);
    }
    return render_transition_metric<Metric::Escape,
        IterResultField::Iter | IterResultField::Escaped>(q, out);
}

} // namespace fsd::compute
