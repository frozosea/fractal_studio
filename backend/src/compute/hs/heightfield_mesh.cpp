// compute/hs/heightfield_mesh.cpp

#include "heightfield_mesh.hpp"
#include "../complex.hpp"
#include "../parallel.hpp"
#include "../orbit_program.hpp"

#ifdef _OPENMP
#  include <omp.h>
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace fsd::compute::hs {

namespace {

template <Metric M>
double hs_raw_value(const IterResult& r, double heightClamp, int iterations) {
    if constexpr (M == Metric::Escape) {
        return r.escaped
            ? static_cast<double>(r.iter) / static_cast<double>(iterations)
            : 1.0;
    } else if constexpr (M == Metric::MinAbs) {
        return std::isfinite(r.min_abs) ? r.min_abs : heightClamp;
    } else if constexpr (M == Metric::MaxAbs) {
        return r.max_abs;
    } else if constexpr (M == Metric::Envelope) {
        return 0.5 * (r.min_abs + r.max_abs);
    } else {
        return std::isfinite(r.extra) ? r.extra : heightClamp;
    }
}

// Populate `field` with per-pixel metric values using the same iterate core as
// the map kernel. Field is row-major: field[row * N + col].
template <Variant V, Metric M, IterResultMask NeedMask>
void computeFieldMetricImpl(const HsMeshParams& p, std::vector<double>& field) {
    const int N = p.resolution;
    field.assign(N * N, 0.0);
    const double span = p.scale;
    const double re_min = p.center_re - span * 0.5;
    const double im_max = p.center_im + span * 0.5;
    const double bail2 = p.bailout_sq;
    const int thread_count = resolve_render_threads(p.render_threads);
    const int pairwise_cap = std::max(1, std::min(p.iterations, p.pairwise_cap));
    constexpr int tile_size = 32;
    const int tiles = (N + tile_size - 1) / tile_size;
    const int tile_count = tiles * tiles;

    #pragma omp parallel num_threads(thread_count)
    {
        std::vector<Cx<double>> orbit_scratch;
        if constexpr (M == Metric::MinPairwiseDist) {
            orbit_scratch.reserve(static_cast<size_t>(pairwise_cap));
        }

        #pragma omp for schedule(dynamic, 1)
        for (int tile = 0; tile < tile_count; tile++) {
            if (p.should_cancel && p.should_cancel()) continue;
            const int tile_col = tile % tiles;
            const int tile_row = tile / tiles;
            const int row0 = tile_row * tile_size;
            const int col0 = tile_col * tile_size;
            const int row1 = std::min(N, row0 + tile_size);
            const int col1 = std::min(N, col0 + tile_size);

            for (int row = row0; row < row1; row++) {
                const double im = im_max - (static_cast<double>(row) + 0.5) / N * span;
                for (int col = col0; col < col1; col++) {
                    const double re = re_min + (static_cast<double>(col) + 0.5) / N * span;
                    Cx<double> c{re, im};
                    Cx<double> z0{0.0, 0.0};
                    IterResult r;
                    if constexpr (M == Metric::MinPairwiseDist) {
                        r = iterate_pairwise<V, double>(
                            z0, c, p.iterations, p.bailout, bail2, pairwise_cap, orbit_scratch);
                    } else {
                        r = iterate_masked<NeedMask, V, double>(
                            z0, c, p.iterations, p.bailout, bail2);
                    }
                    double v = hs_raw_value<M>(r, p.heightClamp, p.iterations);
                    if (v > p.heightClamp) v = p.heightClamp;
                    if (!std::isfinite(v)) v = p.heightClamp;
                    field[row * N + col] = v;
                }
            }
        }
    }
}

template <Variant V>
void computeFieldImpl(const HsMeshParams& p, std::vector<double>& field) {
    switch (p.metric) {
        case Metric::Escape:
            computeFieldMetricImpl<V, Metric::Escape,
                IterResultField::Iter | IterResultField::Escaped>(p, field); break;
        case Metric::MinAbs:
            computeFieldMetricImpl<V, Metric::MinAbs, IterResultField::MinAbs>(p, field); break;
        case Metric::MaxAbs:
            computeFieldMetricImpl<V, Metric::MaxAbs, IterResultField::MaxAbs>(p, field); break;
        case Metric::Envelope:
            computeFieldMetricImpl<V, Metric::Envelope,
                IterResultField::MinAbs | IterResultField::MaxAbs>(p, field); break;
        case Metric::MinPairwiseDist:
            computeFieldMetricImpl<V, Metric::MinPairwiseDist, IterResultField::Extra>(p, field); break;
        case Metric::MandelShipAgree:
            break;
    }
}

void computeOrbitField(const HsMeshParams& p, std::vector<double>& field) {
    if (p.metric == Metric::MandelShipAgree) {
        throw std::runtime_error("Orbit Program does not support mandel_ship_agree HS metric");
    }
    const int N = p.resolution;
    field.assign(static_cast<size_t>(N) * N, 0.0);
    const double span = p.scale;
    const double re_min = p.center_re - span * 0.5;
    const double im_max = p.center_im + span * 0.5;
    const bool certified = p.orbit_program->escape_analysis().has_finite_radius();
    const double radius = certified ? p.orbit_program->escape_analysis().certified_radius : 0.0;
    const double radius_sq = radius * radius;
    const int pairwise_cap = std::max(1, std::min(p.iterations, p.pairwise_cap));
    const int thread_count = resolve_render_threads(p.render_threads);
    constexpr int tile_size = 32;
    const int tiles = (N + tile_size - 1) / tile_size;

    #pragma omp parallel num_threads(thread_count)
    {
        std::vector<Cx<double>> orbit;
        orbit.reserve(static_cast<size_t>(pairwise_cap));
        #pragma omp for schedule(dynamic, 1)
        for (int tile = 0; tile < tiles * tiles; ++tile) {
            if (p.should_cancel && p.should_cancel()) continue;
            const int row0 = (tile / tiles) * tile_size;
            const int col0 = (tile % tiles) * tile_size;
            const int row1 = std::min(N, row0 + tile_size);
            const int col1 = std::min(N, col0 + tile_size);
            for (int row = row0; row < row1; ++row) {
                const double im = im_max - (static_cast<double>(row) + 0.5) / N * span;
                for (int col = col0; col < col1; ++col) {
                    const Cx<double> c{
                        re_min + (static_cast<double>(col) + 0.5) / N * span, im};
                    Cx<double> z{0.0, 0.0};
                    int escape_iteration = p.iterations;
                    bool escaped = false;
                    double min_norm_sq = std::numeric_limits<double>::infinity();
                    double max_norm_sq = 0.0;
                    double min_pair_sq = std::numeric_limits<double>::infinity();
                    orbit.clear();

                    for (int iteration = 0; iteration < p.iterations; ++iteration) {
                        if ((iteration & 63) == 0 && p.should_cancel && p.should_cancel()) break;
                        z = p.orbit_program->step(z, c, iteration);
                        if (!std::isfinite(z.re) || !std::isfinite(z.im)) break;
                        const double norm_sq = z.re * z.re + z.im * z.im;
                        if (!std::isfinite(norm_sq)) break;
                        min_norm_sq = std::min(min_norm_sq, norm_sq);
                        max_norm_sq = std::max(max_norm_sq, norm_sq);
                        if (p.metric == Metric::MinPairwiseDist &&
                            static_cast<int>(orbit.size()) < pairwise_cap) {
                            for (const Cx<double>& prior : orbit) {
                                const double dr = z.re - prior.re;
                                const double di = z.im - prior.im;
                                min_pair_sq = std::min(min_pair_sq, dr * dr + di * di);
                            }
                            orbit.push_back(z);
                        }
                        if (certified && norm_sq > radius_sq) {
                            escaped = true;
                            escape_iteration = iteration;
                            break;
                        }
                        if (p.metric == Metric::MinPairwiseDist && iteration + 1 >= pairwise_cap) break;
                    }

                    double value = p.heightClamp;
                    switch (p.metric) {
                        case Metric::Escape:
                            value = escaped
                                ? static_cast<double>(escape_iteration) / p.iterations
                                : 1.0;
                            break;
                        case Metric::MinAbs:
                            if (std::isfinite(min_norm_sq)) value = std::sqrt(min_norm_sq);
                            break;
                        case Metric::MaxAbs:
                            if (std::isfinite(max_norm_sq)) value = std::sqrt(max_norm_sq);
                            break;
                        case Metric::Envelope:
                            if (std::isfinite(min_norm_sq) && std::isfinite(max_norm_sq)) {
                                value = 0.5 * (std::sqrt(min_norm_sq) + std::sqrt(max_norm_sq));
                            }
                            break;
                        case Metric::MinPairwiseDist:
                            if (std::isfinite(min_pair_sq)) value = std::sqrt(min_pair_sq);
                            break;
                        case Metric::MandelShipAgree:
                            break;
                    }
                    if (!std::isfinite(value) || value > p.heightClamp) value = p.heightClamp;
                    if (value < 0.0) value = 0.0;
                    field[static_cast<size_t>(row) * N + col] = value;
                }
            }
        }
    }
}

} // namespace

// Public: compute raw metric field. Used by both buildHsMesh and hsFieldRoute.
void computeHsField(const HsMeshParams& p, std::vector<double>& field) {
    if (p.orbit_program) {
        computeOrbitField(p, field);
        if (p.should_cancel && p.should_cancel()) throw std::runtime_error("cancelled");
        return;
    }
    using V = Variant;
    switch (p.variant) {
        case V::Mandelbrot: computeFieldImpl<V::Mandelbrot>(p, field); break;
        case V::Tri:        computeFieldImpl<V::Tri>       (p, field); break;
        case V::Boat:       computeFieldImpl<V::Boat>      (p, field); break;
        case V::Duck:       computeFieldImpl<V::Duck>      (p, field); break;
        case V::Bell:       computeFieldImpl<V::Bell>      (p, field); break;
        case V::Fish:       computeFieldImpl<V::Fish>      (p, field); break;
        case V::Vase:       computeFieldImpl<V::Vase>      (p, field); break;
        case V::Bird:       computeFieldImpl<V::Bird>      (p, field); break;
        case V::Mask:       computeFieldImpl<V::Mask>      (p, field); break;
        case V::Ship:       computeFieldImpl<V::Ship>      (p, field); break;
        case V::SinZ:       computeFieldImpl<V::SinZ>      (p, field); break;
        case V::CosZ:       computeFieldImpl<V::CosZ>      (p, field); break;
        case V::ExpZ:       computeFieldImpl<V::ExpZ>      (p, field); break;
        case V::SinhZ:      computeFieldImpl<V::SinhZ>     (p, field); break;
        case V::CoshZ:      computeFieldImpl<V::CoshZ>     (p, field); break;
        case V::TanZ:       computeFieldImpl<V::TanZ>      (p, field); break;
        default:            computeFieldImpl<V::Mandelbrot>(p, field); break;
    }
    if (p.should_cancel && p.should_cancel()) throw std::runtime_error("cancelled");
}

Mesh buildHsMesh(const HsMeshParams& p) {
    const int N = std::max(4, std::min(4096, p.resolution));

    std::vector<double> field;
    HsMeshParams pp = p;
    pp.resolution = N;
    computeHsField(pp, field);
    if (p.should_cancel && p.should_cancel()) throw std::runtime_error("cancelled");

    // Normalize field to [0, 1] for visual consistency.
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    for (double v : field) {
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    const double denom = (hi > lo) ? (hi - lo) : 1.0;

    Mesh mesh;
    mesh.vertices.resize(static_cast<size_t>(N) * static_cast<size_t>(N));
    mesh.indices.reserve(static_cast<size_t>((N - 1) * (N - 1) * 6));

    const double heightScale = p.heightScale;

    for (int row = 0; row < N; row++) {
        for (int col = 0; col < N; col++) {
            const double u = static_cast<double>(col) / static_cast<double>(N - 1) - 0.5;
            const double v = static_cast<double>(row) / static_cast<double>(N - 1) - 0.5;
            const double f01 = (field[row * N + col] - lo) / denom;
            mesh.vertices[row * N + col] = Vec3{
                static_cast<float>(u),
                static_cast<float>(-v),  // flip so row 0 is +Y (image convention)
                static_cast<float>(f01 * heightScale),
            };
        }
    }

    for (int row = 0; row < N - 1; row++) {
        for (int col = 0; col < N - 1; col++) {
            const uint32_t a = static_cast<uint32_t>(row       * N + col);
            const uint32_t b = static_cast<uint32_t>(row       * N + col + 1);
            const uint32_t c = static_cast<uint32_t>((row + 1) * N + col);
            const uint32_t d = static_cast<uint32_t>((row + 1) * N + col + 1);
            // Two triangles per cell, consistent winding (CCW when viewed
            // from +Z).
            mesh.indices.push_back(a);
            mesh.indices.push_back(c);
            mesh.indices.push_back(b);
            mesh.indices.push_back(b);
            mesh.indices.push_back(c);
            mesh.indices.push_back(d);
        }
    }

    // ── Closed-base: north/south/east/west walls + bottom ────────────────
    // Coordinate system recap: X=col/(N-1)−0.5, Y=−(row/(N-1)−0.5), Z=f01×heightScale
    // top surface winding is CCW from +Z so top normals point +Z.
    // Walls and bottom need outward normals; winding verified by cross-product.

    auto topX = [&](int col) -> float {
        return static_cast<float>(static_cast<double>(col) / static_cast<double>(N - 1) - 0.5);
    };
    auto topY = [&](int row) -> float {
        return static_cast<float>(-(static_cast<double>(row) / static_cast<double>(N - 1) - 0.5));
    };

    // North wall (row=0, Y=+0.5) — outward normal +Y
    {
        const float yw = topY(0);
        const uint32_t bBase = static_cast<uint32_t>(mesh.vertices.size());
        for (int col = 0; col < N; col++)
            mesh.vertices.push_back({topX(col), yw, 0.0f});
        for (int col = 0; col < N - 1; col++) {
            const uint32_t tc  = static_cast<uint32_t>(col);
            const uint32_t tc1 = static_cast<uint32_t>(col + 1);
            const uint32_t bc  = bBase + col;
            const uint32_t bc1 = bBase + col + 1;
            mesh.indices.push_back(tc);  mesh.indices.push_back(tc1); mesh.indices.push_back(bc);
            mesh.indices.push_back(tc1); mesh.indices.push_back(bc1); mesh.indices.push_back(bc);
        }
    }

    // South wall (row=N-1, Y=−0.5) — outward normal −Y (reversed winding)
    {
        const float yw = topY(N - 1);
        const uint32_t bBase = static_cast<uint32_t>(mesh.vertices.size());
        for (int col = 0; col < N; col++)
            mesh.vertices.push_back({topX(col), yw, 0.0f});
        for (int col = 0; col < N - 1; col++) {
            const uint32_t tc  = static_cast<uint32_t>((N - 1) * N + col);
            const uint32_t tc1 = static_cast<uint32_t>((N - 1) * N + col + 1);
            const uint32_t bc  = bBase + col;
            const uint32_t bc1 = bBase + col + 1;
            mesh.indices.push_back(tc1); mesh.indices.push_back(tc);  mesh.indices.push_back(bc1);
            mesh.indices.push_back(tc);  mesh.indices.push_back(bc);  mesh.indices.push_back(bc1);
        }
    }

    // East wall (col=N-1, X=+0.5) — outward normal +X
    {
        const float xw = topX(N - 1);
        const uint32_t bBase = static_cast<uint32_t>(mesh.vertices.size());
        for (int row = 0; row < N; row++)
            mesh.vertices.push_back({xw, topY(row), 0.0f});
        for (int row = 0; row < N - 1; row++) {
            const uint32_t tc  = static_cast<uint32_t>(row       * N + (N - 1));
            const uint32_t tc1 = static_cast<uint32_t>((row + 1) * N + (N - 1));
            const uint32_t bc  = bBase + row;
            const uint32_t bc1 = bBase + row + 1;
            mesh.indices.push_back(tc);  mesh.indices.push_back(tc1); mesh.indices.push_back(bc);
            mesh.indices.push_back(tc1); mesh.indices.push_back(bc1); mesh.indices.push_back(bc);
        }
    }

    // West wall (col=0, X=−0.5) — outward normal −X (reversed winding)
    {
        const float xw = topX(0);
        const uint32_t bBase = static_cast<uint32_t>(mesh.vertices.size());
        for (int row = 0; row < N; row++)
            mesh.vertices.push_back({xw, topY(row), 0.0f});
        for (int row = 0; row < N - 1; row++) {
            const uint32_t tc  = static_cast<uint32_t>(row       * N);
            const uint32_t tc1 = static_cast<uint32_t>((row + 1) * N);
            const uint32_t bc  = bBase + row;
            const uint32_t bc1 = bBase + row + 1;
            mesh.indices.push_back(tc1); mesh.indices.push_back(tc);  mesh.indices.push_back(bc1);
            mesh.indices.push_back(tc);  mesh.indices.push_back(bc);  mesh.indices.push_back(bc1);
        }
    }

    // Bottom face (Z=0) — outward normal −Z (CW from above)
    {
        const uint32_t bNW = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({-0.5f,  0.5f, 0.0f}); // NW
        const uint32_t bNE = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({ 0.5f,  0.5f, 0.0f}); // NE
        const uint32_t bSE = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({ 0.5f, -0.5f, 0.0f}); // SE
        const uint32_t bSW = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({-0.5f, -0.5f, 0.0f}); // SW
        mesh.indices.push_back(bNW); mesh.indices.push_back(bNE); mesh.indices.push_back(bSE);
        mesh.indices.push_back(bNW); mesh.indices.push_back(bSE); mesh.indices.push_back(bSW);
    }

    return mesh;
}

} // namespace fsd::compute::hs
