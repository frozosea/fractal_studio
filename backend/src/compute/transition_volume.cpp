// compute/transition_volume.cpp

#include "transition_volume.hpp"
#include "engine_select.hpp"
#include "parallel.hpp"
#include "transition_volume_avx2.hpp"

#if defined(HAS_CUDA_KERNEL)
#  include "cuda/transition_volume.cuh"
#  define USE_CUDA_TRANSITION 1
#else
#  define USE_CUDA_TRANSITION 0
#endif

#ifdef _OPENMP
#  include <omp.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fsd::compute {

namespace {

bool transition_cancel_requested(const TransitionVolumeParams& p) {
    return p.should_cancel && p.should_cancel();
}

void throw_if_transition_cancelled(const TransitionVolumeParams& p) {
    if (transition_cancel_requested(p)) throw std::runtime_error("cancelled");
}

inline float transition_real_projection_f32(Variant v, float x2, float axis2) {
    const bool post_abs =
        v == Variant::Fish || v == Variant::Vase || v == Variant::Bird ||
        v == Variant::Mask || v == Variant::Ship;
    float q = x2 - axis2;
    if (post_abs) q = std::fabs(q);
    return q;
}

inline float transition_imag_projection_f32(Variant v, float x, float axis) {
    const bool abs_x =
        v == Variant::Boat || v == Variant::Bell ||
        v == Variant::Bird || v == Variant::Ship;
    const bool abs_axis =
        v == Variant::Boat || v == Variant::Duck || v == Variant::Mask || v == Variant::Bird;
    const bool neg =
        v == Variant::Tri || v == Variant::Bell || v == Variant::Vase || v == Variant::Ship;
    float a = abs_x ? std::fabs(x) : x;
    float b = abs_axis ? std::fabs(axis) : axis;
    float q = 2.0f * a * b;
    return neg ? -q : q;
}

constexpr int MAX_TRANSITION_LEGS = 4;

struct ActiveVolumeLeg {
    Variant variant = Variant::Mandelbrot;
    float y_factor = 1.0f;
    float z_factor = 0.0f;
    float influence = 1.0f;
};

std::vector<ActiveVolumeLeg> active_volume_legs(const std::vector<TransitionLeg>& input) {
    if (input.size() > MAX_TRANSITION_LEGS) {
        throw std::runtime_error("multi transition supports at most 4 variants");
    }

    std::vector<TransitionLeg> kept;
    kept.reserve(input.size());
    double max_w = 0.0;
    double sum_w2 = 0.0;
    for (const TransitionLeg& leg : input) {
        if (!variant_supports_axis_transition(leg.variant)) {
            throw std::runtime_error("transition variants must be quadratic Mandelbrot-family variants");
        }
        if (!std::isfinite(leg.weight)) throw std::runtime_error("invalid transition weight");
        if (leg.weight <= 0.0) continue;
        kept.push_back(leg);
        max_w = std::max(max_w, leg.weight);
        sum_w2 += leg.weight * leg.weight;
    }
    if (kept.empty() || max_w <= 0.0 || sum_w2 <= 0.0) {
        throw std::runtime_error("multi transition needs at least one positive-weight variant");
    }

    const int n = static_cast<int>(kept.size());
    const double rms_w = std::sqrt(sum_w2 / static_cast<double>(n));
    constexpr double PI = 3.14159265358979323846264338327950288;
    std::vector<ActiveVolumeLeg> out;
    out.reserve(kept.size());
    for (int i = 0; i < n; ++i) {
        double by = 1.0;
        double bz = 0.0;
        if (n == 2) {
            by = (i == 0) ? 1.0 : 0.0;
            bz = (i == 0) ? 0.0 : 1.0;
        } else if (n > 2) {
            const double angle = 2.0 * PI * static_cast<double>(i) / static_cast<double>(n);
            const double scale = std::sqrt(2.0 / static_cast<double>(n));
            by = std::cos(angle) * scale;
            bz = std::sin(angle) * scale;
        }

        const double axis_scale = kept[static_cast<size_t>(i)].weight / rms_w;
        out.push_back({
            kept[static_cast<size_t>(i)].variant,
            static_cast<float>(by * axis_scale),
            static_cast<float>(bz * axis_scale),
            static_cast<float>(kept[static_cast<size_t>(i)].weight / max_w),
        });
    }
    return out;
}

std::string select_transition_engine(const TransitionVolumeParams& p) {
    const RuntimeCapabilities caps = runtime_capabilities();
    const long long voxels = static_cast<long long>(p.resolution) * p.resolution * p.resolution;
    const long long work = voxels * static_cast<long long>(std::max(1, p.iterations));
    const bool large = work >= 250000000LL;

    if (p.engine == "cuda") return caps.cuda_runtime ? "cuda" : "openmp";
    if (p.engine == "avx2") return (caps.avx2_compiled && caps.avx2_runtime && caps.fma_runtime) ? "avx2" : "openmp";
    if (p.engine == "avx512") return "openmp";
    if (p.engine == "hybrid") return caps.cuda_runtime ? "hybrid" : "openmp";
    if (p.engine == "openmp") return "openmp";

    if (large && caps.cuda_runtime && !caps.cuda_low_end) return "hybrid";
    if (caps.avx2_compiled && caps.avx2_runtime && caps.fma_runtime) return "avx2";
    return "openmp";
}

void build_transition_range_openmp(const TransitionVolumeParams& p, int N, int z_begin, int z_end, McField& field) {
    const float span = static_cast<float>(p.extent * 2.0);
    const float xmin = static_cast<float>(p.centerX - p.extent);
    const float ymin = static_cast<float>(p.centerY - p.extent);
    const float zmin = static_cast<float>(p.centerZ - p.extent);
    const float bail2 = static_cast<float>(p.bailout_sq);
    const float bailout = static_cast<float>(p.bailout);
    const int maxIter = p.iterations;

    for (int zi = z_begin; zi < z_end; zi++) {
        if (transition_cancel_requested(p)) continue;
        const float z0 = zmin + (static_cast<float>(zi) + 0.5f) / static_cast<float>(N) * span;
        for (int yi = 0; yi < N; yi++) {
            const float y0 = ymin + (static_cast<float>(yi) + 0.5f) / static_cast<float>(N) * span;
            for (int xi = 0; xi < N; xi++) {
                const float x0 = xmin + (static_cast<float>(xi) + 0.5f) / static_cast<float>(N) * span;

                float x = x0, y = y0, z = z0;
                float x2 = x * x;
                float y2 = y * y;
                float z2 = z * z;
                int iter = 0;
                bool escaped = false;
                for (; iter < maxIter; iter++) {
                    if ((iter & 63) == 0 && transition_cancel_requested(p)) break;
                    const float nx =
                        transition_real_projection_f32(p.from_variant, x2, y2)
                      + transition_real_projection_f32(p.to_variant,   x2, z2)
                      - x2 + x0;
                    const float ny = transition_imag_projection_f32(p.from_variant, x, y) + y0;
                    const float nz = transition_imag_projection_f32(p.to_variant,   x, z) + z0;
                    const bool finite_xyz = std::isfinite(nx) && std::isfinite(ny) && std::isfinite(nz);
                    const float nx2 = finite_xyz ? nx * nx : std::numeric_limits<float>::infinity();
                    const float ny2 = finite_xyz ? ny * ny : std::numeric_limits<float>::infinity();
                    const float nz2 = finite_xyz ? nz * nz : std::numeric_limits<float>::infinity();
                    const float n2 = finite_xyz
                        ? (nx2 + ny2 + nz2)
                        : std::numeric_limits<float>::infinity();
                    if (!finite_xyz || n2 > bail2) {
                        escaped = true;
                        break;
                    }
                    x = nx; y = ny; z = nz;
                    x2 = nx2; y2 = ny2; z2 = nz2;
                }

                float v = 0.0f;
                if (escaped) {
                    v = 0.5f + 0.5f * (static_cast<float>(iter) / static_cast<float>(maxIter));
                } else {
                    const float mag2 = x2 + y2 + z2;
                    const float finalMag = std::isfinite(mag2) ? std::sqrt(mag2) : bailout;
                    v = (finalMag / bailout) * 0.48f;
                }

                const size_t idx = static_cast<size_t>(xi) +
                                   static_cast<size_t>(N) *
                                   (static_cast<size_t>(yi) + static_cast<size_t>(N) * static_cast<size_t>(zi));
                field.data[idx] = v;
            }
        }
    }
}

void build_transition_range_multi_openmp(const TransitionVolumeParams& p, int N, int z_begin, int z_end, McField& field) {
    const std::vector<ActiveVolumeLeg> legs = active_volume_legs(p.multi_legs);
    const int leg_count = static_cast<int>(legs.size());

    const float span = static_cast<float>(p.extent * 2.0);
    const float xmin = static_cast<float>(p.centerX - p.extent);
    const float ymin = static_cast<float>(p.centerY - p.extent);
    const float zmin = static_cast<float>(p.centerZ - p.extent);
    const float bail2 = static_cast<float>(p.bailout_sq);
    const float bailout = static_cast<float>(p.bailout);
    const int maxIter = p.iterations;

    for (int zi = z_begin; zi < z_end; zi++) {
        if (transition_cancel_requested(p)) continue;
        const float z0 = zmin + (static_cast<float>(zi) + 0.5f) / static_cast<float>(N) * span;
        for (int yi = 0; yi < N; yi++) {
            const float y0 = ymin + (static_cast<float>(yi) + 0.5f) / static_cast<float>(N) * span;
            for (int xi = 0; xi < N; xi++) {
                const float x0 = xmin + (static_cast<float>(xi) + 0.5f) / static_cast<float>(N) * span;

                float x = x0;
                float x2 = x * x;
                std::array<float, MAX_TRANSITION_LEGS> axis{};
                std::array<float, MAX_TRANSITION_LEGS> axis2{};
                for (int k = 0; k < leg_count; ++k) {
                    axis[k] = y0 * legs[static_cast<size_t>(k)].y_factor
                            + z0 * legs[static_cast<size_t>(k)].z_factor;
                    axis2[k] = axis[k] * axis[k];
                }

                int iter = 0;
                bool escaped = false;
                for (; iter < maxIter; iter++) {
                    if ((iter & 63) == 0 && transition_cancel_requested(p)) break;
                    float real_sum = 0.0f;
                    float influence_sum = 0.0f;
                    std::array<float, MAX_TRANSITION_LEGS> next_axis{};
                    for (int k = 0; k < leg_count; ++k) {
                        const ActiveVolumeLeg& leg = legs[static_cast<size_t>(k)];
                        real_sum += leg.influence * transition_real_projection_f32(leg.variant, x2, axis2[k]);
                        influence_sum += leg.influence;
                        const float caxis = y0 * leg.y_factor + z0 * leg.z_factor;
                        next_axis[k] = leg.influence * transition_imag_projection_f32(leg.variant, x, axis[k]) + caxis;
                    }

                    const float nx = real_sum - (influence_sum - 1.0f) * x2 + x0;
                    const bool finite_x = std::isfinite(nx);
                    const float nx2 = finite_x ? nx * nx : std::numeric_limits<float>::infinity();
                    bool finite_all = finite_x;
                    float n2 = finite_x ? nx2 : std::numeric_limits<float>::infinity();
                    for (int k = 0; k < leg_count; ++k) {
                        if (!std::isfinite(next_axis[k])) {
                            finite_all = false;
                            n2 = std::numeric_limits<float>::infinity();
                            break;
                        }
                        n2 += next_axis[k] * next_axis[k];
                    }
                    if (!finite_all || n2 > bail2) {
                        escaped = true;
                        break;
                    }

                    x = nx;
                    x2 = nx2;
                    for (int k = 0; k < leg_count; ++k) {
                        axis[k] = next_axis[k];
                        axis2[k] = axis[k] * axis[k];
                    }
                }

                float v = 0.0f;
                if (escaped) {
                    v = 0.5f + 0.5f * (static_cast<float>(iter) / static_cast<float>(maxIter));
                } else {
                    float mag2 = x2;
                    for (int k = 0; k < leg_count; ++k) mag2 += axis2[k];
                    const float finalMag = std::isfinite(mag2) ? std::sqrt(mag2) : bailout;
                    v = (finalMag / bailout) * 0.48f;
                }

                const size_t idx = static_cast<size_t>(xi) +
                                   static_cast<size_t>(N) *
                                   (static_cast<size_t>(yi) + static_cast<size_t>(N) * static_cast<size_t>(zi));
                field.data[idx] = v;
            }
        }
    }
}

#if USE_CUDA_TRANSITION
fsd_cuda::CudaTransitionVolumeParams make_cuda_transition_params(const TransitionVolumeParams& p, int N) {
    fsd_cuda::CudaTransitionVolumeParams cp;
    cp.center_x = static_cast<float>(p.centerX);
    cp.center_y = static_cast<float>(p.centerY);
    cp.center_z = static_cast<float>(p.centerZ);
    cp.extent = static_cast<float>(p.extent);
    cp.resolution = N;
    cp.iterations = p.iterations;
    cp.bailout = static_cast<float>(p.bailout);
    cp.bailout_sq = static_cast<float>(p.bailout_sq);
    cp.from_variant = static_cast<int>(p.from_variant);
    cp.to_variant = static_cast<int>(p.to_variant);

    if (!p.multi_legs.empty()) {
        const std::vector<ActiveVolumeLeg> legs = active_volume_legs(p.multi_legs);
        cp.multi_count = static_cast<int>(legs.size());
        for (int i = 0; i < cp.multi_count; ++i) {
            cp.multi_variants[i] = static_cast<int>(legs[static_cast<size_t>(i)].variant);
            cp.multi_y_factor[i] = legs[static_cast<size_t>(i)].y_factor;
            cp.multi_z_factor[i] = legs[static_cast<size_t>(i)].z_factor;
            cp.multi_influence[i] = legs[static_cast<size_t>(i)].influence;
        }
    }

    return cp;
}

bool build_transition_hybrid_cuda_cpu(const TransitionVolumeParams& p, int N, McField& field) {
    fsd_cuda::CudaTransitionVolumeParams cp = make_cuda_transition_params(p, N);

    std::atomic<int> next_z{0};
    std::atomic<int> gpu_slabs{0};
    std::atomic<int> cpu_slabs{0};
    std::atomic<bool> gpu_available{true};
    const RuntimeCapabilities caps = runtime_capabilities();
    std::atomic<bool> cpu_avx2_available{
        caps.avx2_compiled && caps.avx2_runtime && caps.fma_runtime
    };
    const int gpu_batch = runtime_capabilities().cuda_low_end ? 4 : 12;

    auto render_cpu_range = [&](int z0, int zCount) {
        if (cpu_avx2_available.load(std::memory_order_relaxed)) {
            if (buildTransitionVolumeAvx2Range(p, N, z0, z0 + zCount, field)) {
                return;
            }
            cpu_avx2_available.store(false, std::memory_order_relaxed);
        }
        build_transition_range_openmp(p, N, z0, z0 + zCount, field);
    };

    auto copy_slabs = [&](int z0, int zCount, const std::vector<float>& tmp) {
        const size_t slab = static_cast<size_t>(N) * N;
        for (int local = 0; local < zCount; ++local) {
            const size_t dst = slab * static_cast<size_t>(z0 + local);
            const size_t src = slab * static_cast<size_t>(local);
            std::copy(tmp.begin() + static_cast<std::ptrdiff_t>(src),
                      tmp.begin() + static_cast<std::ptrdiff_t>(src + slab),
                      field.data.begin() + static_cast<std::ptrdiff_t>(dst));
        }
    };

    std::thread gpu_thread([&]() {
        while (true) {
            if (transition_cancel_requested(p)) break;
            const int z0 = next_z.fetch_add(gpu_batch);
            if (z0 >= N) break;
            const int zCount = std::min(gpu_batch, N - z0);
            if (!gpu_available.load(std::memory_order_relaxed)) {
                render_cpu_range(z0, zCount);
                cpu_slabs += zCount;
                continue;
            }
            try {
                std::vector<float> tmp;
                fsd_cuda::cuda_build_transition_volume_slabs(cp, z0, zCount, tmp);
                copy_slabs(z0, zCount, tmp);
                gpu_slabs += zCount;
            } catch (...) {
                gpu_available.store(false, std::memory_order_relaxed);
                render_cpu_range(z0, zCount);
                cpu_slabs += zCount;
            }
        }
    });

    const int cpu_threads = default_render_threads();
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(cpu_threads));
    for (int i = 0; i < cpu_threads; ++i) {
        workers.emplace_back([&]() {
            while (true) {
                if (transition_cancel_requested(p)) break;
                const int z0 = next_z.fetch_add(1);
                if (z0 >= N) break;
                render_cpu_range(z0, 1);
                cpu_slabs++;
            }
        });
    }

    for (auto& worker : workers) worker.join();
    if (gpu_thread.joinable()) gpu_thread.join();

    if (gpu_slabs.load() > 0 && cpu_slabs.load() > 0) {
        if (!p.multi_legs.empty()) {
            field.engine_used = cpu_avx2_available.load(std::memory_order_relaxed)
                ? "hybrid_cuda_avx2_multi_fp32"
                : "hybrid_cuda_openmp_multi_fp32";
        } else {
            field.engine_used = cpu_avx2_available.load(std::memory_order_relaxed)
                ? "hybrid_cuda_avx2_fp32"
                : "hybrid_cuda_openmp_fp32";
        }
        return true;
    }
    if (gpu_slabs.load() > 0) {
        field.engine_used = p.multi_legs.empty() ? "cuda_fp32" : "cuda_multi_fp32";
        return true;
    }
    if (!p.multi_legs.empty()) {
        field.engine_used = cpu_avx2_available.load(std::memory_order_relaxed)
            ? "avx2_multi_fp32"
            : "openmp_multi_fp32";
    } else {
        field.engine_used = cpu_avx2_available.load(std::memory_order_relaxed)
            ? "avx2_fp32"
            : "openmp_fp32";
    }
    return cpu_slabs.load() > 0;
}
#endif

} // namespace

McField buildTransitionVolume(const TransitionVolumeParams& p) {
    throw_if_transition_cancelled(p);
    if (p.multi_legs.empty() && (!variant_supports_axis_transition(p.from_variant) ||
        !variant_supports_axis_transition(p.to_variant))) {
        throw std::runtime_error("transition variants must be quadratic Mandelbrot-family variants");
    }
    if (!p.multi_legs.empty()) {
        (void)active_volume_legs(p.multi_legs);
    }

    const int N = std::max(4, std::min(1024, p.resolution));
    McField field;
    field.Nx = field.Ny = field.Nz = N;
    field.data.assign(static_cast<size_t>(N) * N * N, 1.0f);
    field.scalar_used = "fp32";
    const std::string selected_engine = select_transition_engine(p);
    field.engine_used = selected_engine == "openmp" ? "openmp_fp32" : selected_engine + "_fp32_openmp_fallback";

#if USE_CUDA_TRANSITION
    if (selected_engine == "hybrid") {
        if (build_transition_hybrid_cuda_cpu(p, N, field)) {
            throw_if_transition_cancelled(p);
            return field;
        }
        field.data.assign(static_cast<size_t>(N) * N * N, 1.0f);
        field.engine_used = "hybrid_fp32_openmp_fallback";
    }

    if (selected_engine == "cuda") {
        try {
            fsd_cuda::CudaTransitionVolumeParams cp = make_cuda_transition_params(p, N);
            fsd_cuda::cuda_build_transition_volume(cp, field.data);
            throw_if_transition_cancelled(p);
            field.engine_used = p.multi_legs.empty() ? "cuda_fp32" : "cuda_multi_fp32";
            return field;
        } catch (...) {
            field.data.assign(static_cast<size_t>(N) * N * N, 1.0f);
            field.engine_used = selected_engine + "_fp32_openmp_fallback";
        }
    }
#endif

    if (selected_engine == "avx2") {
        if (buildTransitionVolumeAvx2(p, field)) {
            throw_if_transition_cancelled(p);
            return field;
        }
        field.data.assign(static_cast<size_t>(N) * N * N, 1.0f);
        field.engine_used = "avx2_fp32_openmp_fallback";
    }

    const int thread_count = default_render_threads();

    #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 1)
    for (int zi = 0; zi < N; zi++) {
        if (transition_cancel_requested(p)) continue;
        if (p.multi_legs.empty()) {
            build_transition_range_openmp(p, N, zi, zi + 1, field);
        } else {
            build_transition_range_multi_openmp(p, N, zi, zi + 1, field);
        }
    }

    throw_if_transition_cancelled(p);

    if (!p.multi_legs.empty()) field.engine_used = "openmp_multi_fp32";

    return field;
}

} // namespace fsd::compute
