// compute/ln_map.cpp

#include "ln_map.hpp"

#include "engine_select.hpp"
#include "escape_time.hpp"
#include "fx64_raw.hpp"
#include "map_kernel_avx2.hpp"
#include "map_kernel_avx512.hpp"
#include "parallel.hpp"
#include "perturbation.hpp"
#include "perturbation_avx2.hpp"
#include "perturbation_avx512.hpp"

#if defined(HAS_CUDA_KERNEL)
#  include "cuda/ln_map.cuh"
#  include "cuda/map_kernel.cuh"      // cuda_available()
#  include "cuda/perturb_kernel.cuh"
#  define USE_CUDA_LN_MAP 1
#  define USE_CUDA 1
#else
#  define USE_CUDA_LN_MAP 0
#  define USE_CUDA 0
#endif

#include <opencv2/core.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace fsd::compute {

// Deep-zoom perturbation providers (defined with the perturbation renderer
// near the bottom of this file, after the anonymous namespace).
static bool lnmap_perturbation_applicable(const LnMapParams& p);
static std::string compute_ln_perturb_iters(const LnMapParams& p,
                                            std::vector<int>& iters,
                                            const LnMapProgress& on_row_done);

namespace {

constexpr double TAU = 6.283185307179586;
constexpr double LN_TWO = 0.6931471805599453;
constexpr double LN_FOUR = 1.3862943611198906;

struct TrigColumns {
    std::vector<double> cos_col;
    std::vector<double> sin_col;
};

void ensure_ln_out(const LnMapParams& p, cv::Mat& out) {
    if (out.empty() || out.rows != p.height_t || out.cols != p.width_s || out.type() != CV_8UC3) {
        out.create(p.height_t, p.width_s, CV_8UC3);
    }
}

std::pair<int, int> clamp_rows(const LnMapParams& p, int row_start, int row_count) {
    const int start = std::max(0, std::min(row_start, p.height_t));
    const int end = std::max(start, std::min(p.height_t, start + std::max(0, row_count)));
    return {start, end};
}

TrigColumns make_trig_columns(int s) {
    TrigColumns cols;
    cols.cos_col.resize(static_cast<size_t>(s));
    cols.sin_col.resize(static_cast<size_t>(s));
    for (int x = 0; x < s; x++) {
        const double th = TAU * static_cast<double>(x) / static_cast<double>(s);
        cols.cos_col[static_cast<size_t>(x)] = std::cos(th);
        cols.sin_col[static_cast<size_t>(x)] = std::sin(th);
    }
    return cols;
}

bool ln_map_fast_mode(const LnMapParams& p) {
    return p.precision_mode == "fast";
}

bool ln_map_scalar_is_fx64(const LnMapParams& p) {
    return p.scalar_type == "fx64" || p.scalar_type == "q6.57" ||
           p.scalar_type == "q657" || p.scalar_type == "fixed57";
}

bool engine_allows_cuda(const LnMapParams& p) {
    return p.engine == "auto" || p.engine == "cuda" || p.engine == "hybrid";
}

bool ln_map_colormap_wraps_phase(Colormap colormap) {
    return colormap == Colormap::ClassicCos ||
           colormap == Colormap::HsvWheel ||
           colormap == Colormap::Tri765 ||
           colormap == Colormap::Twilight ||
           colormap == Colormap::Spectral1530;
}

double apply_ln_map_depth_phase(double mapped, double phase, Colormap colormap) {
    mapped = std::clamp(mapped, 0.0, 1.0);
    if (colormap == Colormap::Grayscale || colormap == Colormap::Mod17) {
        return mapped;
    }
    if (ln_map_colormap_wraps_phase(colormap)) {
        return std::fmod(mapped + phase, 1.0);
    }
    return std::clamp(mapped + phase * (1.0 - mapped), 0.0, 1.0);
}

int rows_for_depth_octaves(const LnMapParams& p, double depth_octaves) {
    if (!(depth_octaves > 0.0) || !std::isfinite(depth_octaves)) return 0;
    const double rows = depth_octaves * LN_TWO / TAU * static_cast<double>(p.width_s);
    return std::clamp(static_cast<int>(std::ceil(rows)), 0, p.height_t);
}

template <typename RenderFn, typename NotifyFn>
void render_layer_parallel(
    int row_start,
    int row_count,
    int batch_rows,
    RenderFn render_chunk,
    NotifyFn notify
) {
    if (row_count <= 0) return;
    const int end = row_start + row_count;
    const int batch = std::max(1, batch_rows);
    std::atomic<int> next_row{row_start};
    const int thread_count = default_render_threads();
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(thread_count));
    for (int i = 0; i < thread_count; ++i) {
        workers.emplace_back([&]() {
            while (true) {
                const int row0 = next_row.fetch_add(batch, std::memory_order_relaxed);
                if (row0 >= end) break;
                const int rows = std::min(batch, end - row0);
                render_chunk(row0, rows);
                notify(rows);
            }
        });
    }
    for (auto& worker : workers) worker.join();
}

template <Variant V>
void render_ln_variant_openmp_rows_impl(
    const LnMapParams& p,
    cv::Mat& out,
    int row_start,
    int row_end,
    const TrigColumns& cols,
    bool threaded,
    const LnMapProgress& on_row_done
) {
    const Cx<double> c_julia{p.julia_re, p.julia_im};
    const int s = p.width_s;
    std::atomic<int> rows_done{0};

    auto render_row = [&](int row) {
        uint8_t* rowp = out.ptr<uint8_t>(row);
        const double global_row = p.row_offset + static_cast<double>(row);
        const double k = LN_FOUR - global_row * TAU / static_cast<double>(s);
        const double r_mag = std::exp(k);
        for (int x = 0; x < s; x++) {
            const double pre = p.center_re + r_mag * cols.cos_col[static_cast<size_t>(x)];
            const double pim = p.center_im + r_mag * cols.sin_col[static_cast<size_t>(x)];
            Cx<double> z0, c;
            if (p.julia) {
                z0 = {pre, pim};
                c = c_julia;
            } else {
                z0 = {0.0, 0.0};
                c = {pre, pim};
            }
            const IterResult ir = iterate_masked<
                IterResultField::Iter | IterResultField::Escaped,
                V, double>(z0, c, p.iterations, p.bailout, p.bailout_sq);
            uint8_t* px = rowp + 3 * x;
            const int it = ir.escaped ? ir.iter : p.iterations;
            colorize_escape_bgr(it, p.iterations, p.colormap, 0.0, false, px[0], px[1], px[2]);
        }
        if (on_row_done) {
            const int done = rows_done.fetch_add(1, std::memory_order_relaxed) + 1;
            if (done == row_end - row_start || (done % 16) == 0) on_row_done(done);
        }
    };

    if (threaded) {
        const int thread_count = default_render_threads();
        #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 8)
        for (int row = row_start; row < row_end; row++) {
            render_row(row);
        }
    } else {
        for (int row = row_start; row < row_end; row++) {
            render_row(row);
        }
    }
}

void dispatch_openmp_rows_impl(
    const LnMapParams& p,
    cv::Mat& out,
    int row_start,
    int row_end,
    const TrigColumns& cols,
    bool threaded,
    const LnMapProgress& on_row_done
) {
    switch (p.variant) {
        case Variant::Mandelbrot: render_ln_variant_openmp_rows_impl<Variant::Mandelbrot>(p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::Tri:        render_ln_variant_openmp_rows_impl<Variant::Tri>       (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::Boat:       render_ln_variant_openmp_rows_impl<Variant::Boat>      (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::Duck:       render_ln_variant_openmp_rows_impl<Variant::Duck>      (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::Bell:       render_ln_variant_openmp_rows_impl<Variant::Bell>      (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::Fish:       render_ln_variant_openmp_rows_impl<Variant::Fish>      (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::Vase:       render_ln_variant_openmp_rows_impl<Variant::Vase>      (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::Bird:       render_ln_variant_openmp_rows_impl<Variant::Bird>      (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::Mask:       render_ln_variant_openmp_rows_impl<Variant::Mask>      (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::Ship:       render_ln_variant_openmp_rows_impl<Variant::Ship>      (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::SinZ:       render_ln_variant_openmp_rows_impl<Variant::SinZ>      (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::CosZ:       render_ln_variant_openmp_rows_impl<Variant::CosZ>      (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::ExpZ:       render_ln_variant_openmp_rows_impl<Variant::ExpZ>      (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::SinhZ:      render_ln_variant_openmp_rows_impl<Variant::SinhZ>     (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::CoshZ:      render_ln_variant_openmp_rows_impl<Variant::CoshZ>     (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::TanZ:       render_ln_variant_openmp_rows_impl<Variant::TanZ>      (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::Custom:     throw std::runtime_error("ln-map custom variants are not supported");
    }
}

template <int FRAC, Variant V>
void render_ln_variant_fixed_rows_impl(
    const LnMapParams& p,
    cv::Mat& out,
    int row_start,
    int row_end,
    const TrigColumns& cols,
    bool threaded,
    const LnMapProgress& on_row_done
) {
    using S = Fixed64<FRAC>;
    const Cx<S> c_julia{S::from_double(p.julia_re), S::from_double(p.julia_im)};
    const int s = p.width_s;
    const uint64_t bailout_raw = fixed_round_to_uraw_sat<FRAC>(p.bailout);
    const uint64_t bailout2_raw = fixed_round_to_uraw_sat<FRAC>(p.bailout_sq);
    const uint64_t two_raw = fixed_two_raw_const<FRAC>();
    const uint64_t two_sqrt2_floor_raw = fixed_two_sqrt2_floor_raw_const<FRAC>();
    std::atomic<int> rows_done{0};

    auto render_row = [&](int row) {
        uint8_t* rowp = out.ptr<uint8_t>(row);
        const double global_row = p.row_offset + static_cast<double>(row);
        const double k = LN_FOUR - global_row * TAU / static_cast<double>(s);
        const double r_mag = std::exp(k);
        for (int x = 0; x < s; x++) {
            const double pre = p.center_re + r_mag * cols.cos_col[static_cast<size_t>(x)];
            const double pim = p.center_im + r_mag * cols.sin_col[static_cast<size_t>(x)];
            Cx<S> z0, c;
            if (p.julia) {
                z0 = {S::from_double(pre), S::from_double(pim)};
                c = c_julia;
            } else {
                z0 = {S{0}, S{0}};
                c = {S::from_double(pre), S::from_double(pim)};
            }
            const IterResult ir = iterate_fixed_int_masked<
                FRAC,
                FixedEscapeGate::Direct,
                IterResultField::Iter | IterResultField::Escaped,
                V>(z0, c, p.iterations, bailout_raw, bailout2_raw,
                   two_raw, two_sqrt2_floor_raw, false);
            uint8_t* px = rowp + 3 * x;
            const int it = ir.escaped ? ir.iter : p.iterations;
            colorize_escape_bgr(it, p.iterations, p.colormap, 0.0, false, px[0], px[1], px[2]);
        }
        if (on_row_done) {
            const int done = rows_done.fetch_add(1, std::memory_order_relaxed) + 1;
            if (done == row_end - row_start || (done % 16) == 0) on_row_done(done);
        }
    };

    if (threaded) {
        const int thread_count = default_render_threads();
        #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 4)
        for (int row = row_start; row < row_end; row++) {
            render_row(row);
        }
    } else {
        for (int row = row_start; row < row_end; row++) {
            render_row(row);
        }
    }
}

template <int FRAC>
void dispatch_fixed_rows_impl(
    const LnMapParams& p,
    cv::Mat& out,
    int row_start,
    int row_end,
    const TrigColumns& cols,
    bool threaded,
    const LnMapProgress& on_row_done
) {
    switch (p.variant) {
        case Variant::Mandelbrot: render_ln_variant_fixed_rows_impl<FRAC, Variant::Mandelbrot>(p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::Tri:        render_ln_variant_fixed_rows_impl<FRAC, Variant::Tri>       (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::Boat:       render_ln_variant_fixed_rows_impl<FRAC, Variant::Boat>      (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::Duck:       render_ln_variant_fixed_rows_impl<FRAC, Variant::Duck>      (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::Bell:       render_ln_variant_fixed_rows_impl<FRAC, Variant::Bell>      (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::Fish:       render_ln_variant_fixed_rows_impl<FRAC, Variant::Fish>      (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::Vase:       render_ln_variant_fixed_rows_impl<FRAC, Variant::Vase>      (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::Bird:       render_ln_variant_fixed_rows_impl<FRAC, Variant::Bird>      (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::Mask:       render_ln_variant_fixed_rows_impl<FRAC, Variant::Mask>      (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::Ship:       render_ln_variant_fixed_rows_impl<FRAC, Variant::Ship>      (p, out, row_start, row_end, cols, threaded, on_row_done); break;
        case Variant::SinZ:
        case Variant::CosZ:
        case Variant::ExpZ:
        case Variant::SinhZ:
        case Variant::CoshZ:
        case Variant::TanZ:
            dispatch_openmp_rows_impl(p, out, row_start, row_end, cols, threaded, on_row_done);
            break;
        case Variant::Custom:
            throw std::runtime_error("ln-map custom variants are not supported");
    }
}

LnMapStats render_ln_map_openmp_fx64_rows(
    const LnMapParams& p,
    cv::Mat& out,
    int row_start,
    int row_count,
    const LnMapProgress& on_row_done
) {
    ensure_ln_out(p, out);
    const auto [start, end] = clamp_rows(p, row_start, row_count);
    const auto t0 = std::chrono::steady_clock::now();
    const TrigColumns cols = make_trig_columns(p.width_s);
    dispatch_fixed_rows_impl<57>(p, out, start, end, cols, true, on_row_done);
    const auto t1 = std::chrono::steady_clock::now();
    LnMapStats stats;
    stats.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    stats.pixel_count = p.width_s * (end - start);
    stats.engine_used = "openmp";
    stats.scalar_used = ln_map_variant_supported_by_simd(p.variant) ? "fx64" : "fp64";
    stats.precision_mode = p.precision_mode;
    return stats;
}

LnMapStats render_ln_map_openmp_fx64(const LnMapParams& p, cv::Mat& out, const LnMapProgress& on_row_done) {
    return render_ln_map_openmp_fx64_rows(p, out, 0, p.height_t, on_row_done);
}

#if USE_CUDA_LN_MAP
fsd_cuda::CudaLnMapParams make_cuda_params(const LnMapParams& p) {
    fsd_cuda::CudaLnMapParams cp;
    cp.julia = p.julia;
    cp.center_re = p.center_re;
    cp.center_im = p.center_im;
    cp.julia_re = p.julia_re;
    cp.julia_im = p.julia_im;
    cp.width_s = p.width_s;
    cp.height_t = p.height_t;
    cp.row_offset = p.row_offset;
    cp.iterations = p.iterations;
    cp.bailout = p.bailout;
    cp.bailout_sq = p.bailout_sq;
    cp.variant_id = static_cast<int>(p.variant);
    cp.colormap_id = static_cast<int>(p.colormap);
    return cp;
}

int cuda_progress_chunk_rows(const LnMapParams& p) {
    const int h = std::max(1, p.height_t);
    const bool low_end = runtime_capabilities().cuda_low_end;
    const int target_updates = low_end ? 48 : 64;
    const int min_rows = low_end ? 8 : 16;
    const int max_rows = low_end ? 64 : 256;
    return std::clamp((h + target_updates - 1) / target_updates, min_rows, max_rows);
}

LnMapStats render_ln_map_cuda_with_progress(
    const LnMapParams& p,
    cv::Mat& out,
    const LnMapProgress& on_row_done
) {
    ensure_ln_out(p, out);
    const fsd_cuda::CudaLnMapParams cp = make_cuda_params(p);
    const int chunk_rows = cuda_progress_chunk_rows(p);

    double elapsed_ms = 0.0;
    for (int row0 = 0; row0 < p.height_t; row0 += chunk_rows) {
        const int rows = std::min(chunk_rows, p.height_t - row0);
        const auto stats = fsd_cuda::cuda_render_ln_map_rows(cp, out, row0, rows);
        elapsed_ms += stats.elapsed_ms;
        if (on_row_done) on_row_done(row0 + rows);
    }

    LnMapStats stats;
    stats.elapsed_ms = elapsed_ms;
    stats.pixel_count = p.width_s * p.height_t;
    stats.engine_used = "cuda";
    stats.scalar_used = "fp64";
    stats.precision_mode = p.precision_mode;
    return stats;
}

double render_cuda_fp32_rows_with_progress(
    const LnMapParams& p,
    cv::Mat& out,
    int row_start,
    int row_count,
    const LnMapProgress& on_rows_done
) {
    ensure_ln_out(p, out);
    const fsd_cuda::CudaLnMapParams cp = make_cuda_params(p);
    const int chunk_rows = cuda_progress_chunk_rows(p);
    double elapsed_ms = 0.0;
    const int row_end = row_start + row_count;
    for (int row0 = row_start; row0 < row_end; row0 += chunk_rows) {
        const int rows = std::min(chunk_rows, row_end - row0);
        const auto stats = fsd_cuda::cuda_render_ln_map_fp32_rows(cp, out, row0, rows);
        elapsed_ms += stats.elapsed_ms;
        if (on_rows_done) on_rows_done(rows);
    }
    return elapsed_ms;
}

double render_cuda_fx64_rows_with_progress(
    const LnMapParams& p,
    cv::Mat& out,
    int row_start,
    int row_count,
    const LnMapProgress& on_rows_done
) {
    ensure_ln_out(p, out);
    const fsd_cuda::CudaLnMapParams cp = make_cuda_params(p);
    const int chunk_rows = cuda_progress_chunk_rows(p);
    double elapsed_ms = 0.0;
    const int row_end = row_start + row_count;
    for (int row0 = row_start; row0 < row_end; row0 += chunk_rows) {
        const int rows = std::min(chunk_rows, row_end - row0);
        const auto stats = fsd_cuda::cuda_render_ln_map_fx64_rows(cp, out, row0, rows);
        elapsed_ms += stats.elapsed_ms;
        if (on_rows_done) on_rows_done(rows);
    }
    return elapsed_ms;
}

LnMapStats render_ln_map_cuda_fx64_with_progress(
    const LnMapParams& p,
    cv::Mat& out,
    const LnMapProgress& on_row_done
) {
    int rows_done = 0;
    const auto notify = [&](int delta) {
        rows_done += delta;
        if (on_row_done) on_row_done(std::min(rows_done, p.height_t));
    };
    const double elapsed_ms = render_cuda_fx64_rows_with_progress(p, out, 0, p.height_t, notify);
    LnMapStats stats;
    stats.elapsed_ms = elapsed_ms;
    stats.pixel_count = p.width_s * p.height_t;
    stats.engine_used = "cuda";
    stats.scalar_used = "fx64";
    stats.precision_mode = p.precision_mode;
    return stats;
}
#endif

bool should_try_cuda(const LnMapParams& p) {
#if USE_CUDA_LN_MAP
    if (!ln_map_variant_supported_by_simd(p.variant)) return false;
    if (p.engine == "cuda" || p.engine == "hybrid") return fsd_cuda::cuda_ln_map_available();
    if (p.engine != "auto") return false;
    const auto caps = runtime_capabilities();
    return caps.cuda_runtime && !caps.cuda_low_end;
#else
    (void)p;
    return false;
#endif
}

bool should_try_cuda_fx64(const LnMapParams& p) {
#if USE_CUDA_LN_MAP
    if (!ln_map_variant_supported_by_simd(p.variant)) return false;
    if (!ln_map_scalar_is_fx64(p)) return false;
    if (p.engine == "cuda" || p.engine == "hybrid") return fsd_cuda::cuda_ln_map_available();
    if (p.engine != "auto") return false;
    return runtime_capabilities().cuda_runtime && fsd_cuda::cuda_ln_map_available();
#else
    (void)p;
    return false;
#endif
}

bool should_try_avx512(const LnMapParams& p) {
    if (!ln_map_variant_supported_by_simd(p.variant)) return false;
    if (p.engine == "avx512") return avx512_available();
    if (p.engine != "auto") return false;
    return avx512_available();
}

bool should_try_avx2(const LnMapParams& p) {
    if (!ln_map_variant_supported_by_simd(p.variant)) return false;
    if (p.engine == "avx2") return ln_map_avx2_available();
    if (p.engine != "auto") return false;
    return ln_map_avx2_available();
}

enum class LnCpuBackend {
    Avx512,
    Avx2,
    Openmp,
};

LnCpuBackend select_ln_cpu_backend(const LnMapParams& p) {
    if (p.engine == "openmp") return LnCpuBackend::Openmp;
    if (p.engine == "avx512") {
        return (ln_map_variant_supported_by_simd(p.variant) && avx512_available())
            ? LnCpuBackend::Avx512
            : LnCpuBackend::Openmp;
    }
    if (p.engine == "avx2") {
        return (ln_map_variant_supported_by_simd(p.variant) && ln_map_avx2_available())
            ? LnCpuBackend::Avx2
            : LnCpuBackend::Openmp;
    }
    if (ln_map_variant_supported_by_simd(p.variant) && avx512_available()) return LnCpuBackend::Avx512;
    if (ln_map_variant_supported_by_simd(p.variant) && ln_map_avx2_available()) return LnCpuBackend::Avx2;
    return LnCpuBackend::Openmp;
}

LnCpuBackend select_ln_cpu_fp32_backend(const LnMapParams& p) {
    if (!ln_map_variant_supported_by_simd(p.variant)) return LnCpuBackend::Openmp;
    if (p.engine == "avx512") return avx512_available() ? LnCpuBackend::Avx512 : LnCpuBackend::Openmp;
    if (p.engine == "avx2") return ln_map_avx2_available() ? LnCpuBackend::Avx2 : LnCpuBackend::Openmp;
    if (p.engine == "openmp") return LnCpuBackend::Openmp;
    if (avx512_available()) return LnCpuBackend::Avx512;
    if (ln_map_avx2_available()) return LnCpuBackend::Avx2;
    return LnCpuBackend::Openmp;
}

const char* ln_cpu_backend_name(LnCpuBackend backend) {
    switch (backend) {
        case LnCpuBackend::Avx512: return "avx512";
        case LnCpuBackend::Avx2: return "avx2";
        case LnCpuBackend::Openmp: return "openmp";
    }
    return "openmp";
}

void render_ln_cpu_rows_serial(
    const LnMapParams& p,
    cv::Mat& out,
    int row_start,
    int row_count,
    LnCpuBackend backend
) {
    if (row_count <= 0) return;
    if (ln_map_scalar_is_fx64(p) && ln_map_variant_supported_by_simd(p.variant)) {
        const auto [start, end] = clamp_rows(p, row_start, row_count);
        const TrigColumns cols = make_trig_columns(p.width_s);
        dispatch_fixed_rows_impl<57>(p, out, start, end, cols, false, nullptr);
        return;
    }
    switch (backend) {
        case LnCpuBackend::Avx512:
            render_ln_map_avx512_rows(p, out, row_start, row_count, nullptr);
            return;
        case LnCpuBackend::Avx2:
            render_ln_map_avx2_rows(p, out, row_start, row_count, nullptr);
            return;
        case LnCpuBackend::Openmp: {
            const auto [start, end] = clamp_rows(p, row_start, row_count);
            const TrigColumns cols = make_trig_columns(p.width_s);
            dispatch_openmp_rows_impl(p, out, start, end, cols, false, nullptr);
            return;
        }
    }
}

void render_ln_cpu_fp32_rows_serial(
    const LnMapParams& p,
    cv::Mat& out,
    int row_start,
    int row_count,
    LnCpuBackend backend
) {
    if (row_count <= 0) return;
    switch (backend) {
        case LnCpuBackend::Avx512:
            render_ln_map_avx512_fp32_rows(p, out, row_start, row_count, nullptr);
            return;
        case LnCpuBackend::Avx2:
            render_ln_map_avx2_fp32_rows(p, out, row_start, row_count, nullptr);
            return;
        case LnCpuBackend::Openmp:
            render_ln_cpu_rows_serial(p, out, row_start, row_count, select_ln_cpu_backend(p));
            return;
    }
}

enum class LnPlanScalar {
    Fp32,
    Fp64,
    Fx64,
};

struct LnRenderBand {
    int start = 0;
    int end = 0;
    LnPlanScalar scalar = LnPlanScalar::Fp64;
};

struct LnValidationResult {
    int samples = 0;
    int mismatch = 0;
    double mismatch_ratio = 0.0;
    double mean_abs_iter_delta = 0.0;
    int p99_abs_iter_delta = 0;
    int max_abs_iter_delta = 0;
    double mean_color_delta = 0.0;
    int max_color_delta = 0;
    bool pass = true;
};

const char* ln_plan_scalar_name(LnPlanScalar scalar) {
    switch (scalar) {
        case LnPlanScalar::Fp32: return "fp32";
        case LnPlanScalar::Fp64: return "fp64";
        case LnPlanScalar::Fx64: return "fx64";
    }
    return "fp64";
}

int sample_position(int index, int count, int limit) {
    if (limit <= 1) return 0;
    if (count <= 1) return limit / 2;
    const double t = static_cast<double>(index) / static_cast<double>(count - 1);
    return std::clamp(static_cast<int>(std::llround(t * static_cast<double>(limit - 1))), 0, limit - 1);
}

void color_for_iter(const LnMapParams& p, int iter, uint8_t bgr[3]) {
    colorize_escape_bgr(iter, p.iterations, p.colormap, 0.0, false, bgr[0], bgr[1], bgr[2]);
}

template <Variant V>
int sample_ln_iter_fp64(const LnMapParams& p, int row, int col) {
    const double th = TAU * static_cast<double>(col) / static_cast<double>(p.width_s);
    const double global_row = p.row_offset + static_cast<double>(row);
    const double k = LN_FOUR - global_row * TAU / static_cast<double>(p.width_s);
    const double r_mag = std::exp(k);
    const double pre = p.center_re + r_mag * std::cos(th);
    const double pim = p.center_im + r_mag * std::sin(th);
    Cx<double> z0, c;
    if (p.julia) {
        z0 = {pre, pim};
        c = {p.julia_re, p.julia_im};
    } else {
        z0 = {0.0, 0.0};
        c = {pre, pim};
    }
    const IterResult ir = iterate_masked<
        IterResultField::Iter | IterResultField::Escaped,
        V, double>(z0, c, p.iterations, p.bailout, p.bailout_sq);
    return ir.escaped ? ir.iter : p.iterations;
}

template <Variant V>
int sample_ln_iter_fp32(const LnMapParams& p, int row, int col) {
    constexpr float tau = static_cast<float>(TAU);
    constexpr float ln_four = static_cast<float>(LN_FOUR);
    const float width_s = static_cast<float>(p.width_s);
    const float th = tau * static_cast<float>(col) / width_s;
    const float global_row = static_cast<float>(p.row_offset + static_cast<double>(row));
    const float k = ln_four - global_row * tau / width_s;
    const float r_mag = std::exp(k);
    const float pre = static_cast<float>(p.center_re) + r_mag * std::cos(th);
    const float pim = static_cast<float>(p.center_im) + r_mag * std::sin(th);
    Cx<float> z0, c;
    if (p.julia) {
        z0 = {pre, pim};
        c = {static_cast<float>(p.julia_re), static_cast<float>(p.julia_im)};
    } else {
        z0 = {0.0f, 0.0f};
        c = {pre, pim};
    }
    const IterResult ir = iterate_masked<
        IterResultField::Iter | IterResultField::Escaped,
        V, float>(z0, c, p.iterations, p.bailout, p.bailout_sq);
    return ir.escaped ? ir.iter : p.iterations;
}

template <Variant V>
int sample_ln_iter_fx64(const LnMapParams& p, int row, int col) {
    using S = Fixed64<57>;
    const double th = TAU * static_cast<double>(col) / static_cast<double>(p.width_s);
    const double global_row = p.row_offset + static_cast<double>(row);
    const double k = LN_FOUR - global_row * TAU / static_cast<double>(p.width_s);
    const double r_mag = std::exp(k);
    const double pre = p.center_re + r_mag * std::cos(th);
    const double pim = p.center_im + r_mag * std::sin(th);
    Cx<S> z0, c;
    if (p.julia) {
        z0 = {S::from_double(pre), S::from_double(pim)};
        c = {S::from_double(p.julia_re), S::from_double(p.julia_im)};
    } else {
        z0 = {S{0}, S{0}};
        c = {S::from_double(pre), S::from_double(pim)};
    }
    const IterResult ir = iterate_fixed_int_masked<
        57,
        FixedEscapeGate::Direct,
        IterResultField::Iter | IterResultField::Escaped,
        V>(z0, c, p.iterations,
           fixed_round_to_uraw_sat<57>(p.bailout),
           fixed_round_to_uraw_sat<57>(p.bailout_sq),
           fixed_two_raw_const<57>(),
           fixed_two_sqrt2_floor_raw_const<57>(),
           false);
    return ir.escaped ? ir.iter : p.iterations;
}

template <Variant V>
int sample_ln_iter_variant(const LnMapParams& p, int row, int col, LnPlanScalar scalar) {
    switch (scalar) {
        case LnPlanScalar::Fp32: return sample_ln_iter_fp32<V>(p, row, col);
        case LnPlanScalar::Fp64: return sample_ln_iter_fp64<V>(p, row, col);
        case LnPlanScalar::Fx64: return sample_ln_iter_fx64<V>(p, row, col);
    }
    return sample_ln_iter_fp64<V>(p, row, col);
}

int sample_ln_iter(const LnMapParams& p, int row, int col, LnPlanScalar scalar) {
    switch (p.variant) {
        case Variant::Mandelbrot: return sample_ln_iter_variant<Variant::Mandelbrot>(p, row, col, scalar);
        case Variant::Tri:        return sample_ln_iter_variant<Variant::Tri>       (p, row, col, scalar);
        case Variant::Boat:       return sample_ln_iter_variant<Variant::Boat>      (p, row, col, scalar);
        case Variant::Duck:       return sample_ln_iter_variant<Variant::Duck>      (p, row, col, scalar);
        case Variant::Bell:       return sample_ln_iter_variant<Variant::Bell>      (p, row, col, scalar);
        case Variant::Fish:       return sample_ln_iter_variant<Variant::Fish>      (p, row, col, scalar);
        case Variant::Vase:       return sample_ln_iter_variant<Variant::Vase>      (p, row, col, scalar);
        case Variant::Bird:       return sample_ln_iter_variant<Variant::Bird>      (p, row, col, scalar);
        case Variant::Mask:       return sample_ln_iter_variant<Variant::Mask>      (p, row, col, scalar);
        case Variant::Ship:       return sample_ln_iter_variant<Variant::Ship>      (p, row, col, scalar);
        default:                  return sample_ln_iter_fp64<Variant::Mandelbrot>(p, row, col);
    }
}

template <Variant V>
void compute_ln_iters_fp64_variant(
    const LnMapParams& p,
    std::vector<int>& iters,
    const TrigColumns& cols,
    const LnMapProgress& on_row_done
) {
    const Cx<double> c_julia{p.julia_re, p.julia_im};
    const int s = p.width_s;
    const int h = p.height_t;
    std::atomic<int> rows_done{0};

    auto compute_row = [&](int row) {
        const double global_row = p.row_offset + static_cast<double>(row);
        const double k = LN_FOUR - global_row * TAU / static_cast<double>(s);
        const double r_mag = std::exp(k);
        const size_t row_offset = static_cast<size_t>(row) * static_cast<size_t>(s);
        for (int x = 0; x < s; x++) {
            const double pre = p.center_re + r_mag * cols.cos_col[static_cast<size_t>(x)];
            const double pim = p.center_im + r_mag * cols.sin_col[static_cast<size_t>(x)];
            Cx<double> z0, c;
            if (p.julia) {
                z0 = {pre, pim};
                c = c_julia;
            } else {
                z0 = {0.0, 0.0};
                c = {pre, pim};
            }
            const IterResult ir = iterate_masked<
                IterResultField::Iter | IterResultField::Escaped,
                V, double>(z0, c, p.iterations, p.bailout, p.bailout_sq);
            iters[row_offset + static_cast<size_t>(x)] = ir.escaped ? ir.iter : p.iterations;
        }
        if (on_row_done) {
            const int done = rows_done.fetch_add(1, std::memory_order_relaxed) + 1;
            if (done == h || (done % 16) == 0) on_row_done(done);
        }
    };

    const int thread_count = default_render_threads();
    #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 4)
    for (int row = 0; row < h; row++) {
        compute_row(row);
    }
}

void compute_ln_iters_fp64(
    const LnMapParams& p,
    std::vector<int>& iters,
    const TrigColumns& cols,
    const LnMapProgress& on_row_done
) {
    switch (p.variant) {
        case Variant::Mandelbrot: compute_ln_iters_fp64_variant<Variant::Mandelbrot>(p, iters, cols, on_row_done); break;
        case Variant::Tri:        compute_ln_iters_fp64_variant<Variant::Tri>       (p, iters, cols, on_row_done); break;
        case Variant::Boat:       compute_ln_iters_fp64_variant<Variant::Boat>      (p, iters, cols, on_row_done); break;
        case Variant::Duck:       compute_ln_iters_fp64_variant<Variant::Duck>      (p, iters, cols, on_row_done); break;
        case Variant::Bell:       compute_ln_iters_fp64_variant<Variant::Bell>      (p, iters, cols, on_row_done); break;
        case Variant::Fish:       compute_ln_iters_fp64_variant<Variant::Fish>      (p, iters, cols, on_row_done); break;
        case Variant::Vase:       compute_ln_iters_fp64_variant<Variant::Vase>      (p, iters, cols, on_row_done); break;
        case Variant::Bird:       compute_ln_iters_fp64_variant<Variant::Bird>      (p, iters, cols, on_row_done); break;
        case Variant::Mask:       compute_ln_iters_fp64_variant<Variant::Mask>      (p, iters, cols, on_row_done); break;
        case Variant::Ship:       compute_ln_iters_fp64_variant<Variant::Ship>      (p, iters, cols, on_row_done); break;
        case Variant::SinZ:       compute_ln_iters_fp64_variant<Variant::SinZ>      (p, iters, cols, on_row_done); break;
        case Variant::CosZ:       compute_ln_iters_fp64_variant<Variant::CosZ>      (p, iters, cols, on_row_done); break;
        case Variant::ExpZ:       compute_ln_iters_fp64_variant<Variant::ExpZ>      (p, iters, cols, on_row_done); break;
        case Variant::SinhZ:      compute_ln_iters_fp64_variant<Variant::SinhZ>     (p, iters, cols, on_row_done); break;
        case Variant::CoshZ:      compute_ln_iters_fp64_variant<Variant::CoshZ>     (p, iters, cols, on_row_done); break;
        case Variant::TanZ:       compute_ln_iters_fp64_variant<Variant::TanZ>      (p, iters, cols, on_row_done); break;
        case Variant::Custom:     throw std::runtime_error("ln-map custom variants are not supported");
    }
}

// Build the shared periodic coloring from the rendered ln-map escape-count field.
// Direct linear mapping: phase = (count - count_min) / period, period chosen so the
// whole strip spans total_octaves*cyclesPerOctave palette cycles. No equalization —
// every escape count is one solid band, uniform width, density ∝ log(magnification).
// Shared tail of every periodic equalization: given a (possibly weighted) escape-count
// histogram and the octave span it covers, anchor the palette period on the MEDIAN escape
// count (not the extreme deepest), so the color spread is even instead of the bulk
// collapsing into the first cycle while only a deep minibrot reaches the rest of the wheel.
// `hist`/`total` are doubles so the same code serves unit-weight and distance-weighted
// histograms. Leaves onset_cycles at its struct default.
LnMapEqualization finalize_equalization(
    const std::vector<double>& hist, double total,
    double total_octaves, double cpo,
    Colormap colormap, bool colormap_wraps
) {
    LnMapEqualization eq;
    eq.colormap = colormap;
    eq.colormap_wraps = colormap_wraps;
    const int iterations = static_cast<int>(hist.size());
    if (total <= 0.0 || iterations <= 0) return eq;  // degenerate → eq.valid stays false

    int count_min = 0;
    for (int i = 0; i < iterations; ++i) { if (hist[static_cast<size_t>(i)] > 0.0) { count_min = i; break; } }
    const double half = total / 2.0;
    int median = count_min;
    double cum = 0.0;
    for (int i = 0; i < iterations; ++i) {
        cum += hist[static_cast<size_t>(i)];
        if (cum >= half) { median = i; break; }
    }
    int count_max = count_min + 2 * (median - count_min);
    if (count_max <= count_min) count_max = count_min + 1;

    const double range = std::max(1.0, static_cast<double>(count_max - count_min));
    const double cycles = std::max(1e-6, total_octaves * cpo);
    eq.count_min = static_cast<double>(count_min);
    eq.period = std::max(1e-6, range / cycles);   // escape counts per palette cycle
    eq.valid = true;
    return eq;
}

struct LnEqHistogram {
    std::vector<double> hist;
    double total = 0.0;
    double first_row = std::numeric_limits<double>::infinity();
    double last_row = -std::numeric_limits<double>::infinity();

    explicit LnEqHistogram(int iterations = 0)
        : hist(static_cast<size_t>(std::max(0, iterations)), 0.0) {}

    void add_iter(int iter) {
        if (iter < 0 || iter >= static_cast<int>(hist.size())) return;
        hist[static_cast<size_t>(iter)] += 1.0;
        total += 1.0;
    }

    void include_row(double row) {
        first_row = std::min(first_row, row);
        last_row = std::max(last_row, row);
    }

    void add(int iter, double row) {
        const double before = total;
        add_iter(iter);
        if (total > before) include_row(row);
    }

    void merge_from(const LnEqHistogram& other) {
        if (hist.size() != other.hist.size()) return;
        for (size_t i = 0; i < hist.size(); ++i) {
            hist[i] += other.hist[i];
        }
        total += other.total;
        if (other.valid()) {
            first_row = std::min(first_row, other.first_row);
            last_row = std::max(last_row, other.last_row);
        }
    }

    bool valid() const {
        return total > 0.0 && std::isfinite(first_row) && std::isfinite(last_row);
    }
};

int histogram_accum_threads(int iterations, int rows) {
    if (iterations <= 0 || rows <= 1) return 1;
    const int desired = std::min(default_render_threads(), rows);
    const uint64_t bytes_per_worker =
        static_cast<uint64_t>(iterations) * sizeof(double) * 2u;
    if (bytes_per_worker == 0) return desired;
    constexpr uint64_t max_local_hist_bytes = 256ull * 1024ull * 1024ull;
    const int memory_limited = static_cast<int>(
        std::max<uint64_t>(1u, max_local_hist_bytes / bytes_per_worker));
    return std::max(1, std::min(desired, memory_limited));
}

void accumulate_ln_map_equalization_histograms(
    const LnMapParams& p,
    const std::vector<int>& iters,
    const TrigColumns& cols,
    LnEqHistogram& disk_hist,
    LnEqHistogram& all_hist
) {
    const int s = p.width_s;
    const int h = p.height_t;
    if (s <= 0 || h <= 0 || p.iterations <= 0) return;
    if (iters.size() != static_cast<size_t>(s) * static_cast<size_t>(h)) return;

    const double center_abs = std::hypot(p.center_re, p.center_im);
    const double disk_eps = 32.0 * std::numeric_limits<double>::epsilon() *
        std::max(1.0, center_abs);

    auto accumulate_rows = [&](int row_begin, int row_end, LnEqHistogram& local_disk, LnEqHistogram& local_all) {
        for (int row = row_begin; row < row_end; ++row) {
            const double global_row = p.row_offset + static_cast<double>(row);
            const double k = LN_FOUR - global_row * TAU / static_cast<double>(s);
            const double r_mag = std::exp(k);
            const size_t row_offset = static_cast<size_t>(row) * static_cast<size_t>(s);

            // Conservative whole-row tests for the |center + r*e^(i theta)| <= 2 disk.
            // Most deep rows are fully inside this disk, so this avoids a geometry check
            // per pixel while preserving the exact per-column path near the boundary.
            const bool disk_all =
                center_abs + r_mag <= 2.0 - disk_eps * (1.0 + r_mag);
            const bool disk_none =
                !disk_all && std::fabs(center_abs - r_mag) > 2.0 + disk_eps * (1.0 + r_mag);

            bool all_row_used = false;
            bool disk_row_used = false;

            if (disk_all) {
                for (int x = 0; x < s; ++x) {
                    const int it = iters[row_offset + static_cast<size_t>(x)];
                    if (it < 0 || it >= p.iterations) continue;
                    local_all.add_iter(it);
                    local_disk.add_iter(it);
                    all_row_used = true;
                    disk_row_used = true;
                }
            } else if (disk_none) {
                for (int x = 0; x < s; ++x) {
                    const int it = iters[row_offset + static_cast<size_t>(x)];
                    if (it < 0 || it >= p.iterations) continue;
                    local_all.add_iter(it);
                    all_row_used = true;
                }
            } else {
                for (int x = 0; x < s; ++x) {
                    const int it = iters[row_offset + static_cast<size_t>(x)];
                    if (it < 0 || it >= p.iterations) continue;
                    local_all.add_iter(it);
                    all_row_used = true;

                    const double re = p.center_re + r_mag * cols.cos_col[static_cast<size_t>(x)];
                    const double im = p.center_im + r_mag * cols.sin_col[static_cast<size_t>(x)];
                    if (re * re + im * im <= 4.0) {
                        local_disk.add_iter(it);
                        disk_row_used = true;
                    }
                }
            }

            if (all_row_used) local_all.include_row(global_row);
            if (disk_row_used) local_disk.include_row(global_row);
        }
    };

    const int thread_count = histogram_accum_threads(p.iterations, h);
    if (thread_count <= 1) {
        accumulate_rows(0, h, disk_hist, all_hist);
        return;
    }

    std::vector<LnEqHistogram> local_disk;
    std::vector<LnEqHistogram> local_all;
    local_disk.reserve(static_cast<size_t>(thread_count));
    local_all.reserve(static_cast<size_t>(thread_count));
    for (int i = 0; i < thread_count; ++i) {
        local_disk.emplace_back(p.iterations);
        local_all.emplace_back(p.iterations);
    }

    std::atomic<int> next_row{0};
    const int batch_rows = std::max(1, std::min(32, (h + thread_count * 4 - 1) / (thread_count * 4)));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(thread_count));
    for (int tid = 0; tid < thread_count; ++tid) {
        workers.emplace_back([&, tid]() {
            while (true) {
                const int row0 = next_row.fetch_add(batch_rows, std::memory_order_relaxed);
                if (row0 >= h) break;
                accumulate_rows(row0, std::min(h, row0 + batch_rows),
                                local_disk[static_cast<size_t>(tid)],
                                local_all[static_cast<size_t>(tid)]);
            }
        });
    }
    for (auto& worker : workers) worker.join();

    for (int tid = 0; tid < thread_count; ++tid) {
        disk_hist.merge_from(local_disk[static_cast<size_t>(tid)]);
        all_hist.merge_from(local_all[static_cast<size_t>(tid)]);
    }
}

void merge_u32_histogram(
    LnEqHistogram& dst,
    const std::vector<uint32_t>& counts,
    const std::vector<uint32_t>& row_flags,
    double row_offset
) {
    if (counts.size() != dst.hist.size()) return;
    double added = 0.0;
    for (size_t i = 0; i < counts.size(); ++i) {
        const uint32_t count = counts[i];
        if (count == 0u) continue;
        dst.hist[i] += static_cast<double>(count);
        added += static_cast<double>(count);
    }
    if (added <= 0.0) return;
    dst.total += added;

    for (size_t r = 0; r < row_flags.size(); ++r) {
        if (row_flags[r] != 0u) {
            dst.include_row(row_offset + static_cast<double>(r));
            break;
        }
    }
    for (size_t r = row_flags.size(); r > 0; --r) {
        if (row_flags[r - 1] != 0u) {
            dst.include_row(row_offset + static_cast<double>(r - 1));
            break;
        }
    }
}

bool try_accumulate_ln_map_equalization_histograms_cuda(
    const LnMapParams& p,
    LnEqHistogram& disk_hist,
    LnEqHistogram& all_hist
) {
#if USE_CUDA_LN_MAP
    if (ln_map_fast_mode(p)) return false;
    if (lnmap_perturbation_applicable(p)) return false;
    if (!ln_map_variant_supported_by_simd(p.variant)) return false;
    if (!engine_allows_cuda(p) || !fsd_cuda::cuda_ln_map_available()) return false;

    const size_t iter_count = static_cast<size_t>(p.iterations);
    const size_t row_count = static_cast<size_t>(p.height_t);
    if (iter_count == 0 || row_count == 0) return false;
    const size_t pixel_count = static_cast<size_t>(p.width_s) * row_count;
    if (pixel_count > static_cast<size_t>(std::numeric_limits<int>::max())) return false;

    std::vector<uint32_t> disk_counts(iter_count, 0u);
    std::vector<uint32_t> all_counts(iter_count, 0u);
    std::vector<uint32_t> disk_rows(row_count, 0u);
    std::vector<uint32_t> all_rows(row_count, 0u);

    try {
        const fsd_cuda::CudaLnMapParams cp = make_cuda_params(p);
        fsd_cuda::cuda_accumulate_ln_map_histograms(
            cp,
            disk_counts.data(),
            all_counts.data(),
            disk_rows.data(),
            all_rows.data(),
            0,
            p.height_t,
            ln_map_scalar_is_fx64(p));
        merge_u32_histogram(disk_hist, disk_counts, disk_rows, p.row_offset);
        merge_u32_histogram(all_hist, all_counts, all_rows, p.row_offset);
        return true;
    } catch (...) {
        if (p.engine == "cuda") throw;
        return false;
    }
#else
    (void)p;
    (void)disk_hist;
    (void)all_hist;
    return false;
#endif
}

LnMapEqualization finalize_ln_map_equalization_histogram(
    const LnMapParams& p,
    const LnEqHistogram& hist
) {
    if (!hist.valid()) return LnMapEqualization{};
    double cpo = p.color_cycles_per_octave;
    if (!(cpo > 0.0) || !std::isfinite(cpo)) cpo = 1.0;

    const double participating_rows = std::max(1.0, hist.last_row - hist.first_row + 1.0);
    const double total_octaves =
        participating_rows * TAU / (static_cast<double>(p.width_s) * LN_TWO);

    return finalize_equalization(hist.hist, hist.total, total_octaves, cpo,
                                 p.colormap, ln_map_colormap_wraps_phase(p.colormap));
}

LnMapGlobalCdf finalize_ln_map_global_cdf_histogram(const LnEqHistogram& hist) {
    LnMapGlobalCdf cdf;
    if (!hist.valid()) return cdf;
    cdf.cumulative.assign(hist.hist.size(), 0ULL);
    unsigned long long cumulative = 0ULL;
    cdf.first_iter = static_cast<int>(hist.hist.size());
    for (size_t i = 0; i < hist.hist.size(); ++i) {
        const double count_d = hist.hist[i];
        const unsigned long long count = count_d > 0.0
            ? static_cast<unsigned long long>(std::llround(count_d))
            : 0ULL;
        if (count > 0ULL && cdf.first_iter == static_cast<int>(hist.hist.size())) {
            cdf.first_iter = static_cast<int>(i);
        }
        cumulative += count;
        cdf.cumulative[i] = cumulative;
    }
    cdf.total = cumulative;
    if (cdf.first_iter == static_cast<int>(hist.hist.size())) cdf.first_iter = 0;
    return cdf;
}

LnMapEqualization build_ln_map_equalization(
    const LnMapParams& p,
    const std::vector<int>& iters,
    const TrigColumns& cols
) {
    const int s = p.width_s;
    const int h = p.height_t;
    if (s <= 0 || h <= 0 || p.iterations <= 0) return LnMapEqualization{};

    // Histogram escape counts over the pixels that actually carry fractal structure: the
    // origin disk |c| <= 2 (the Mandelbrot bounding disk). The shallow lead-in rows
    // (r_mag in (2,4]) and off-disk columns escape at count ~0; counting them would spike
    // count_min/median and corrupt the period. Restricting to |c|<=2 also lets the octave
    // span reflect the *participating* pixel range instead of the strip's padding — so the
    // coloring no longer depends on how many extra lead-in/lead-out octaves the strip has.
    LnEqHistogram disk_hist(p.iterations);
    LnEqHistogram all_hist(p.iterations);
    accumulate_ln_map_equalization_histograms(p, iters, cols, disk_hist, all_hist);

    const LnEqHistogram& selected = disk_hist.valid() ? disk_hist : all_hist;
    return finalize_ln_map_equalization_histogram(p, selected);
}

// Single-entry cache of the (expensive) escape-count field, keyed by geometry only —
// NOT by coloring. Re-coloring the same strip (tuning colorMap / cyclesPerOctave before
// exporting the video) then skips the iteration pass entirely. Shared by /api/map/ln and
// /api/video/export since both go through render_ln_map_mapped.
struct LnFieldCache {
    bool valid = false;
    bool julia = false;
    double center_re = 0, center_im = 0, julia_re = 0, julia_im = 0, bailout = 0, bailout_sq = 0;
    double row_offset = 0.0;
    std::string center_re_str, center_im_str;  // deep-zoom identity beyond double
    int width_s = 0, height_t = 0, iterations = 0, variant = -1;
    std::string precision_mode;  // "standard" or "fast" — different precision → different field
    std::string engine;   // engine that produced the cached field (for UI reporting on reuse)
    std::vector<int> iters;
};
std::mutex g_ln_field_cache_mu;
LnFieldCache g_ln_field_cache;

bool ln_field_cache_matches(const LnMapParams& p, const LnFieldCache& c, size_t pixel_count) {
    return c.valid && c.iters.size() == pixel_count &&
           c.precision_mode == p.precision_mode &&
           c.julia == p.julia && c.center_re == p.center_re && c.center_im == p.center_im &&
           c.center_re_str == p.center_re_str && c.center_im_str == p.center_im_str &&
           c.julia_re == p.julia_re && c.julia_im == p.julia_im &&
           c.bailout == p.bailout && c.bailout_sq == p.bailout_sq &&
           c.row_offset == p.row_offset &&
           c.width_s == p.width_s && c.height_t == p.height_t &&
           c.iterations == p.iterations && c.variant == static_cast<int>(p.variant);
}

// Compute the escape-count field for the mapped color modes using the best engine the
// user selected (CUDA / AVX-512 / AVX2 / OpenMP, fp64 or fx64) instead of always
// degrading to scalar OpenMP. Returns the engine name actually used (so the UI can show
// any degradation). `iters` must be sized width_s*height_t.
std::string compute_ln_field(
    const LnMapParams& p,
    std::vector<int>& iters,
    const TrigColumns& cols,
    const LnMapProgress& on_row_done
) {
    const int h = p.height_t;
    const bool want_fx64 = ln_map_scalar_is_fx64(p);
    const bool simd_variant = ln_map_variant_supported_by_simd(p.variant);

#if USE_CUDA_LN_MAP
    if (simd_variant && engine_allows_cuda(p) && fsd_cuda::cuda_ln_map_available()) {
        try {
            fsd_cuda::cuda_render_ln_map_iters_rows(make_cuda_params(p), iters.data(), 0, h, want_fx64);
            if (on_row_done) on_row_done(h);
            return want_fx64 ? "cuda_fx64" : "cuda_fp64";
        } catch (...) {
            if (p.engine == "cuda") throw;  // explicitly requested CUDA → surface the error
        }
    }
#endif

    // CPU fp64 paths (exact). fx64 without CUDA degrades to fp64 — reported honestly.
    std::string eng;
    if (simd_variant && p.engine != "openmp" && p.engine != "avx2" &&
        render_ln_map_avx512_iters_rows(p, iters.data(), 0, h, on_row_done)) {
        eng = "avx512_fp64";
    } else if (simd_variant && p.engine != "openmp" &&
               render_ln_map_avx2_iters_rows(p, iters.data(), 0, h, on_row_done)) {
        eng = "avx2_fp64";
    } else {
        compute_ln_iters_fp64(p, iters, cols, on_row_done);
        eng = "openmp_fp64";
    }
    if (want_fx64) eng += "(fx64->fp64:no_cuda)";  // degradation surfaced to the UI
    return eng;
}

std::string compute_ln_field_fast(const LnMapParams&, std::vector<int>&, const TrigColumns&, const LnMapProgress&);

LnMapStats render_ln_map_mapped(const LnMapParams& p, cv::Mat& out, const LnMapProgress& on_row_done) {
    ensure_ln_out(p, out);
    const auto t0 = std::chrono::steady_clock::now();
    const int s = p.width_s;
    const int h = p.height_t;
    const size_t pixel_count = static_cast<size_t>(s) * static_cast<size_t>(h);
    const std::string mode = p.color_mode;
    const TrigColumns cols = make_trig_columns(s);
    std::vector<int> iters(pixel_count, p.iterations);

    bool cache_hit = false;
    std::string field_engine;
    {
        std::lock_guard<std::mutex> lk(g_ln_field_cache_mu);
        if (ln_field_cache_matches(p, g_ln_field_cache, pixel_count)) {
            iters = g_ln_field_cache.iters;
            field_engine = g_ln_field_cache.engine;
            cache_hit = true;
        }
    }
    if (cache_hit) {
        if (on_row_done) on_row_done(h);  // field reused → report the row pass as complete
        field_engine = "cached(" + field_engine + ")";
    } else {
        // The escape-count field is the expensive pass; compute it with the engine the
        // user selected (CUDA / AVX-512 / AVX2 / OpenMP, fp64 or fx64), not always scalar
        // OpenMP. Deep strips (innermost radius below fp64's useful range) go through
        // the perturbation renderer regardless of precision mode — it is both exact and
        // fast at depth, replacing the fp32/fp64 band plan. The actual engine is
        // surfaced via stats so the UI can show degradation.
        if (lnmap_perturbation_applicable(p)) {
            field_engine = compute_ln_perturb_iters(p, iters, on_row_done);
        } else if (ln_map_fast_mode(p)) {
            field_engine = compute_ln_field_fast(p, iters, cols, on_row_done);
        } else {
            field_engine = compute_ln_field(p, iters, cols, on_row_done);
        }
        std::lock_guard<std::mutex> lk(g_ln_field_cache_mu);
        g_ln_field_cache = LnFieldCache{true, p.julia, p.center_re, p.center_im, p.julia_re,
                                        p.julia_im, p.bailout, p.bailout_sq,
                                        p.row_offset, p.center_re_str, p.center_im_str,
                                        s, h, p.iterations,
                                        static_cast<int>(p.variant), p.precision_mode,
                                        field_engine, iters};
    }

    // hist_eq → periodic equalized coloring via the shared LUT (also reused by the
    // final cartesian frame for a seamless warp blend). bands/frontier keep the older
    // global-CDF blend below.
    const bool is_hist_eq = mode == "hist_eq";
    LnMapEqualization eq;
    const bool using_external_eq = is_hist_eq && p.equalization_override && p.equalization_override->valid;
    if (is_hist_eq) {
        eq = using_external_eq ? *p.equalization_override : build_ln_map_equalization(p, iters, cols);
    }

    const bool needs_global_cdf = mode == "bands" || mode == "frontier";
    std::vector<unsigned long long> hist;
    unsigned long long total = 0;
    int first_hist_iter = p.iterations;
    LnMapGlobalCdf local_global_cdf;
    const bool using_external_cdf =
        needs_global_cdf &&
        p.global_cdf_override &&
        p.global_cdf_override->valid() &&
        p.global_cdf_override->cumulative.size() == static_cast<size_t>(p.iterations);
    if (needs_global_cdf) {
        if (using_external_cdf) {
            hist = p.global_cdf_override->cumulative;
            total = p.global_cdf_override->total;
            first_hist_iter = p.global_cdf_override->first_iter;
        } else {
            hist.assign(static_cast<size_t>(p.iterations), 0ULL);
            for (int row = 0; row < h; ++row) {
                const double global_row = p.row_offset + static_cast<double>(row);
                const double k = LN_FOUR - global_row * TAU / static_cast<double>(s);
                const double r_mag = std::exp(k);
                const size_t row_offset = static_cast<size_t>(row) * static_cast<size_t>(s);
                for (int x = 0; x < s; ++x) {
                    const double re = p.center_re + r_mag * cols.cos_col[static_cast<size_t>(x)];
                    const double im = p.center_im + r_mag * cols.sin_col[static_cast<size_t>(x)];
                    if (re * re + im * im > 4.0) continue;
                    const int it = iters[row_offset + static_cast<size_t>(x)];
                    if (it >= 0 && it < p.iterations) {
                        hist[static_cast<size_t>(it)] += 1ULL;
                        total += 1ULL;
                    }
                }
            }

            for (int i = 0; i < p.iterations; ++i) {
                if (hist[static_cast<size_t>(i)] > 0ULL) {
                    first_hist_iter = i;
                    break;
                }
            }

            unsigned long long cumulative = 0;
            for (auto& count : hist) {
                cumulative += count;
                count = cumulative;
            }
            local_global_cdf.cumulative = hist;
            local_global_cdf.total = total;
            local_global_cdf.first_iter = first_hist_iter == p.iterations ? 0 : first_hist_iter;
        }
    }

    auto raw_q_for_iter = [&](int it) {
        const double q = (static_cast<double>(it) + 1.0) / (static_cast<double>(p.iterations) + 1.0);
        return std::clamp(q, 0.0, 1.0);
    };

    auto global_q_for_iter = [&](int it) {
        if (total > 0 && it >= 0 && it < p.iterations && !hist.empty()) {
            const double q = std::clamp(
                static_cast<double>(hist[static_cast<size_t>(it)]) / static_cast<double>(total),
                0.0,
                1.0);
            if (q <= 0.0 && first_hist_iter < p.iterations && it < first_hist_iter) {
                const double denom = std::max(1.0e-12, raw_q_for_iter(first_hist_iter));
                const double lower_tail = std::clamp(raw_q_for_iter(it) / denom, 0.0, 1.0);
                return 0.10 * lower_tail;
            }
            return 0.10 + 0.90 * q;
        }
        return raw_q_for_iter(it);
    };

    const int depth_den = std::max(1, h - 1);
    const int thread_count = default_render_threads();
    #pragma omp parallel for num_threads(thread_count) schedule(dynamic, 8)
    for (int row = 0; row < h; ++row) {
        uint8_t* rowp = out.ptr<uint8_t>(row);
        const double depth01 = std::clamp(
            static_cast<double>(row) / static_cast<double>(depth_den),
            0.0,
            1.0);
        const size_t row_offset = static_cast<size_t>(row) * static_cast<size_t>(s);
        std::vector<int> row_iters;
        if (mode == "row_eq") {
            row_iters.reserve(static_cast<size_t>(s));
            for (int x = 0; x < s; ++x) {
                const int it = iters[row_offset + static_cast<size_t>(x)];
                if (it >= 0 && it < p.iterations) row_iters.push_back(it);
            }
            std::sort(row_iters.begin(), row_iters.end());
        }

        auto row_q_for_iter = [&](int it) {
            if (!row_iters.empty() && it >= 0 && it < p.iterations) {
                const auto hi = std::upper_bound(row_iters.begin(), row_iters.end(), it);
                return std::clamp(
                    static_cast<double>(hi - row_iters.begin()) / static_cast<double>(row_iters.size()),
                    0.0,
                    1.0);
            }
            return raw_q_for_iter(it);
        };

        auto blended_global_q_for_iter = [&](int it) {
            return global_q_for_iter(it);
        };

        auto q_at = [&](int rr, int xx) {
            rr = std::clamp(rr, 0, h - 1);
            if (xx < 0) xx += s;
            else if (xx >= s) xx -= s;
            const int nit = iters[static_cast<size_t>(rr) * static_cast<size_t>(s) + static_cast<size_t>(xx)];
            if (nit >= p.iterations) return 1.0;
            return global_q_for_iter(nit);
        };

        for (int x = 0; x < s; ++x) {
            uint8_t* px = rowp + 3 * x;
            const int it = iters[row_offset + static_cast<size_t>(x)];
            if (it >= p.iterations) {
                px[0] = px[1] = px[2] = 255;
                continue;
            }

            double mapped = 0.0;
            if (mode == "log_lift") {
                const double raw = raw_q_for_iter(it);
                const double q = std::log1p(64.0 * raw) / std::log1p(64.0);
                mapped = q * (0.82 + 0.18 * depth01);
                mapped = apply_ln_map_depth_phase(mapped, 0.15 * depth01, p.colormap);
            } else if (mode == "row_eq") {
                const double q = row_q_for_iter(it);
                mapped = q * (0.86 + 0.14 * depth01);
                mapped = apply_ln_map_depth_phase(mapped, 0.12 * depth01, p.colormap);
            } else if (mode == "bands") {
                const double base = blended_global_q_for_iter(it);
                const double raw = raw_q_for_iter(it);
                const double broad = 0.5 + 0.5 * std::sin(TAU * (base * 18.0 + depth01 * 0.72));
                const double fine = 0.5 + 0.5 * std::sin(TAU * (std::sqrt(raw) * 72.0 + depth01 * 0.19));
                mapped = std::clamp(0.70 * base + 0.22 * broad + 0.08 * fine, 0.0, 1.0);
                mapped = apply_ln_map_depth_phase(mapped, 0.08 * depth01, p.colormap);
            } else if (mode == "frontier") {
                const double base = blended_global_q_for_iter(it);
                const double dx = q_at(row, x + 1) - q_at(row, x - 1);
                const double dy = q_at(row + 1, x) - q_at(row - 1, x);
                const double edge = std::clamp(std::sqrt(dx * dx + dy * dy) * 5.0, 0.0, 1.0);
                mapped = std::clamp(0.76 * base + 0.24 * edge, 0.0, 1.0);
                mapped = apply_ln_map_depth_phase(mapped, 0.10 * depth01, p.colormap);
                colorize_field_bgr(mapped, p.colormap, px[0], px[1], px[2]);
                const double lift = 0.34 * edge;
                px[0] = static_cast<uint8_t>(clamp255(static_cast<int>(static_cast<double>(px[0]) * (1.0 - lift) + 255.0 * lift)));
                px[1] = static_cast<uint8_t>(clamp255(static_cast<int>(static_cast<double>(px[1]) * (1.0 - lift) + 255.0 * lift)));
                px[2] = static_cast<uint8_t>(clamp255(static_cast<int>(static_cast<double>(px[2]) * (1.0 - lift) + 255.0 * lift)));
                continue;
            } else {
                // hist_eq → periodic equalized coloring (no depth phase: the N-cycle
                // phase carries the depth motion, and a shared LUT keeps strip and
                // final frame in lockstep). One distinct band per escape count.
                if (eq.valid) {
                    eq.colorize(it, px[0], px[1], px[2]);
                    continue;
                }
                mapped = raw_q_for_iter(it);  // degenerate-strip fallback
            }
            colorize_field_bgr(mapped, p.colormap, px[0], px[1], px[2]);
        }
    }

    const auto t1 = std::chrono::steady_clock::now();
    LnMapStats stats;
    stats.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    stats.pixel_count = p.width_s * p.height_t;
    stats.engine_used = field_engine + "_" + mode;   // e.g. "cuda_fp64_hist_eq" / "perturbation_avx512_hist_eq"
    if (field_engine.find("perturbation") != std::string::npos) {
        stats.scalar_used = "perturbation_fp64";
    } else {
        stats.scalar_used = field_engine.find("fx64") != std::string::npos &&
                            field_engine.find("->fp64") == std::string::npos ? "fx64" : "fp64";
    }
    stats.precision_mode = p.precision_mode;
    std::ostringstream layer;
    layer << "color=" << mode;
    if (is_hist_eq || needs_global_cdf) layer << ",histDomain=|c|<=2";
    if (using_external_eq) layer << ",eq=external";
    if (using_external_cdf) layer << ",cdf=external";
    if (needs_global_cdf) layer << ",histPixels=" << total;
    if (needs_global_cdf) layer << ",underflowTail=raw";
    if (is_hist_eq && eq.valid) {
        layer << ",cyclesPerOctave=" << p.color_cycles_per_octave
              << ",countPeriod=" << eq.period
              << ",countMin=" << eq.count_min
              << ",periodic=" << (eq.colormap_wraps ? "frac" : "triangle");
    }
    if (mode == "row_eq") layer << ",rowLocal=1";
    if (mode == "log_lift") layer << ",curve=log1p64";
    if (mode == "bands") layer << ",bands=18+72";
    if (mode == "frontier") layer << ",edge=gradient";
    stats.layer_summary = layer.str();
    if (is_hist_eq) stats.equalization = std::move(eq);
    if (needs_global_cdf) {
        stats.global_cdf = using_external_cdf ? *p.global_cdf_override : std::move(local_global_cdf);
    }
    return stats;
}

LnValidationResult validate_ln_band(
    const LnMapParams& p,
    int row_start,
    int row_end,
    LnPlanScalar candidate,
    LnPlanScalar reference
) {
    LnValidationResult result;
    if (candidate == reference || row_end <= row_start) return result;

    const int rows = row_end - row_start;
    const int sample_rows = std::clamp(p.fast_validation_sample_rows, 1, std::max(1, rows));
    const int sample_cols = std::clamp(p.fast_validation_sample_cols, 1, std::max(1, p.width_s));
    std::vector<int> iter_deltas;
    iter_deltas.reserve(static_cast<size_t>(sample_rows * sample_cols));
    long long abs_iter_sum = 0;
    long long color_delta_sum = 0;

    for (int ri = 0; ri < sample_rows; ++ri) {
        const int local_row = sample_position(ri, sample_rows, rows);
        const int row = row_start + local_row;
        for (int ci = 0; ci < sample_cols; ++ci) {
            const int base_col = sample_position(ci, sample_cols, p.width_s);
            const int col = (base_col + (row * 17)) % std::max(1, p.width_s);
            const int cand_iter = sample_ln_iter(p, row, col, candidate);
            const int ref_iter = sample_ln_iter(p, row, col, reference);
            const int iter_delta = std::abs(cand_iter - ref_iter);
            uint8_t cand_bgr[3] = {};
            uint8_t ref_bgr[3] = {};
            color_for_iter(p, cand_iter, cand_bgr);
            color_for_iter(p, ref_iter, ref_bgr);
            const int color_delta =
                std::abs(static_cast<int>(cand_bgr[0]) - static_cast<int>(ref_bgr[0])) +
                std::abs(static_cast<int>(cand_bgr[1]) - static_cast<int>(ref_bgr[1])) +
                std::abs(static_cast<int>(cand_bgr[2]) - static_cast<int>(ref_bgr[2]));

            result.samples += 1;
            if (iter_delta != 0) result.mismatch += 1;
            abs_iter_sum += iter_delta;
            color_delta_sum += color_delta;
            result.max_abs_iter_delta = std::max(result.max_abs_iter_delta, iter_delta);
            result.max_color_delta = std::max(result.max_color_delta, color_delta);
            iter_deltas.push_back(iter_delta);
        }
    }

    if (result.samples > 0) {
        std::sort(iter_deltas.begin(), iter_deltas.end());
        const size_t p99_index = std::min(
            iter_deltas.size() - 1,
            static_cast<size_t>(std::ceil(0.99 * static_cast<double>(iter_deltas.size()))) - 1);
        result.p99_abs_iter_delta = iter_deltas[p99_index];
        result.mismatch_ratio = static_cast<double>(result.mismatch) / static_cast<double>(result.samples);
        result.mean_abs_iter_delta = static_cast<double>(abs_iter_sum) / static_cast<double>(result.samples);
        result.mean_color_delta = static_cast<double>(color_delta_sum) / static_cast<double>(result.samples);
    }

    result.pass =
        result.mismatch_ratio <= p.fast_validation_max_mismatch_ratio &&
        result.p99_abs_iter_delta <= p.fast_validation_max_p99_iter_delta &&
        result.mean_color_delta <= p.fast_validation_max_mean_color_delta;
    return result;
}

std::string validation_result_summary(
    int start,
    int end,
    LnPlanScalar from,
    LnPlanScalar to,
    const LnValidationResult& validation
) {
    std::ostringstream os;
    os << ln_plan_scalar_name(from) << "[" << start << "," << end << ")";
    if (from != to) os << "->" << ln_plan_scalar_name(to);
    os << ":m=" << validation.mismatch << "/" << validation.samples
       << ",p99=" << validation.p99_abs_iter_delta
       << ",max=" << validation.max_abs_iter_delta
       << ",rgbMean=" << static_cast<int>(std::llround(validation.mean_color_delta));
    return os.str();
}

void append_band(std::vector<LnRenderBand>& bands, int start, int end, LnPlanScalar scalar) {
    if (end <= start) return;
    if (!bands.empty() && bands.back().end == start && bands.back().scalar == scalar) {
        bands.back().end = end;
        return;
    }
    bands.push_back({start, end, scalar});
}

std::vector<LnRenderBand> make_fast_bands(
    const LnMapParams& p,
    int fp32_end,
    int fp64_end,
    bool use_deep_fx64,
    LnPlanScalar reference,
    std::string& validation_summary
) {
    std::vector<LnRenderBand> bands;
    const int h = p.height_t;
    const int validation_rows = std::max(1, rows_for_depth_octaves(p, p.fast_validation_band_octaves));
    auto add_validation_summary = [&](int row0, int row1, LnPlanScalar from, LnPlanScalar to, const LnValidationResult& validation) {
        if (!validation_summary.empty()) validation_summary += ";";
        validation_summary += validation_result_summary(row0, row1, from, to, validation);
    };

    auto add_segment = [&](int start, int end, LnPlanScalar candidate) {
        for (int row0 = start; row0 < end; row0 += validation_rows) {
            const int row1 = std::min(end, row0 + validation_rows);
            LnPlanScalar selected = candidate;
            if (p.fast_validate && candidate != reference) {
                const LnValidationResult validation = validate_ln_band(p, row0, row1, candidate, reference);
                if (!validation.pass) {
                    selected = candidate == LnPlanScalar::Fp32 ? LnPlanScalar::Fp64 : reference;
                    add_validation_summary(row0, row1, candidate, selected, validation);
                    if (selected != reference && reference == LnPlanScalar::Fx64) {
                        const LnValidationResult promoted_validation = validate_ln_band(p, row0, row1, selected, reference);
                        if (!promoted_validation.pass) {
                            add_validation_summary(row0, row1, selected, LnPlanScalar::Fx64, promoted_validation);
                            selected = LnPlanScalar::Fx64;
                        } else {
                            add_validation_summary(row0, row1, selected, selected, promoted_validation);
                        }
                    }
                } else {
                    add_validation_summary(row0, row1, candidate, candidate, validation);
                }
            }
            append_band(bands, row0, row1, selected);
        }
    };

    add_segment(0, fp32_end, LnPlanScalar::Fp32);
    add_segment(fp32_end, fp64_end, LnPlanScalar::Fp64);
    add_segment(fp64_end, h, use_deep_fx64 ? LnPlanScalar::Fx64 : LnPlanScalar::Fp64);
    if (validation_summary.empty()) {
        validation_summary = p.fast_validate ? "validated=no_candidate_checks" : "disabled";
    }
    return bands;
}

// Layered-precision field pass for the "fast" mode: fp32 shallow → fp64 mid → fx64 deep.
// Uses the same band planning and validation as render_ln_map_fast but writes raw escape
// counts instead of BGR pixels. Returns the engine/layer summary string.
// Falls back to compute_ln_field when SIMD+CUDA are unavailable (OpenMP-only has no
// per-band raw-iter path).
std::string compute_ln_field_fast(
    const LnMapParams& p,
    std::vector<int>& iters,
    const TrigColumns& cols,
    const LnMapProgress& on_row_done
) {
    const int h = p.height_t;
    const bool simd_variant = ln_map_variant_supported_by_simd(p.variant);
    const bool explicit_fp64 = p.scalar_type == "fp64";
    const bool requested_fx64 = ln_map_scalar_is_fx64(p);
    const bool cuda_fast =
#if USE_CUDA_LN_MAP
        simd_variant && engine_allows_cuda(p) && fsd_cuda::cuda_ln_map_available();
#else
        false;
#endif

    if (!simd_variant && !cuda_fast) {
        return compute_ln_field(p, iters, cols, on_row_done);
    }

    const LnCpuBackend fp32_backend = select_ln_cpu_fp32_backend(p);
    const bool cpu_fp32_available = fp32_backend == LnCpuBackend::Avx512 || fp32_backend == LnCpuBackend::Avx2;

    int fp32_end = rows_for_depth_octaves(p, p.fast_fp32_depth_octaves);
    int fp64_end = rows_for_depth_octaves(p, p.fast_fp64_depth_octaves);
    fp64_end = std::max(fp32_end, fp64_end);
    if (!cuda_fast && !cpu_fp32_available) fp32_end = 0;
    const bool use_deep_fx64 = (cuda_fast && !explicit_fp64) || requested_fx64;
    if (!use_deep_fx64) fp64_end = h;
    fp32_end = std::clamp(fp32_end, 0, h);
    fp64_end = std::clamp(fp64_end, fp32_end, h);

    std::atomic<int> rows_done{0};
    std::mutex progress_mu;
    auto notify = [&](int delta) {
        if (delta <= 0) return;
        const int done = rows_done.fetch_add(delta, std::memory_order_relaxed) + delta;
        if (on_row_done) {
            std::lock_guard<std::mutex> lock(progress_mu);
            on_row_done(std::min(done, h));
        }
    };

    const LnPlanScalar reference_scalar = requested_fx64 ? LnPlanScalar::Fx64 : LnPlanScalar::Fp64;
    std::string validation_summary;
    const std::vector<LnRenderBand> bands = make_fast_bands(
        p, fp32_end, fp64_end, use_deep_fx64, reference_scalar, validation_summary);

    std::ostringstream layers;
    bool first_layer = true;
    auto add_layer = [&](int start, int end, const char* scalar, const std::string& engine) {
        if (end <= start) return;
        if (!first_layer) layers << ";";
        first_layer = false;
        layers << scalar << "[" << start << "," << end << ")@" << engine;
    };

    auto render_cuda_iters_band = [&](int start, int end, bool fx64_mode) -> bool {
#if USE_CUDA_LN_MAP
        if (!cuda_fast) return false;
        try {
            const auto cp = make_cuda_params(p);
            const int chunk = cuda_progress_chunk_rows(p);
            for (int r0 = start; r0 < end; r0 += chunk) {
                const int rows = std::min(chunk, end - r0);
                fsd_cuda::cuda_render_ln_map_iters_rows(cp, iters.data(), r0, rows, fx64_mode);
                notify(rows);
            }
            return true;
        } catch (...) {
            if (p.engine == "cuda") throw;
            return false;
        }
#else
        (void)start; (void)end; (void)fx64_mode;
        return false;
#endif
    };

    auto render_simd_iters_band = [&](int start, int band_rows) -> const char* {
        if (p.engine != "openmp" && p.engine != "avx2" &&
            render_ln_map_avx512_iters_rows(p, iters.data(), start, band_rows, nullptr)) {
            return "avx512";
        }
        if (p.engine != "openmp" &&
            render_ln_map_avx2_iters_rows(p, iters.data(), start, band_rows, nullptr)) {
            return "avx2";
        }
        return nullptr;
    };

    for (const LnRenderBand& band : bands) {
        const int band_rows = band.end - band.start;
        if (band_rows <= 0) continue;

        if (band.scalar == LnPlanScalar::Fp32) {
            bool rendered = false;
#if USE_CUDA_LN_MAP
            if (cuda_fast) {
                try {
                    const auto cp = make_cuda_params(p);
                    const int chunk = cuda_progress_chunk_rows(p);
                    for (int r0 = band.start; r0 < band.end; r0 += chunk) {
                        const int rows = std::min(chunk, band.end - r0);
                        fsd_cuda::cuda_render_ln_map_iters_fp32_rows(cp, iters.data(), r0, rows);
                        notify(rows);
                    }
                    add_layer(band.start, band.end, "fp32", "cuda");
                    rendered = true;
                } catch (...) {
                    if (p.engine == "cuda") throw;
                }
            }
#endif
            if (!rendered && cpu_fp32_available) {
                if (fp32_backend == LnCpuBackend::Avx512) {
                    render_ln_map_avx512_fp32_iters_rows(p, iters.data(), band.start, band_rows, nullptr);
                } else {
                    render_ln_map_avx2_fp32_iters_rows(p, iters.data(), band.start, band_rows, nullptr);
                }
                notify(band_rows);
                add_layer(band.start, band.end, "fp32", ln_cpu_backend_name(fp32_backend));
                rendered = true;
            }
            if (!rendered) {
                if (const char* eng = render_simd_iters_band(band.start, band_rows)) {
                    notify(band_rows);
                    add_layer(band.start, band.end, "fp32->fp64", eng);
                } else if (render_cuda_iters_band(band.start, band.end, false)) {
                    add_layer(band.start, band.end, "fp32->fp64", "cuda");
                }
            }
            continue;
        }

        if (band.scalar == LnPlanScalar::Fx64) {
            if (render_cuda_iters_band(band.start, band.end, true)) {
                add_layer(band.start, band.end, "fx64", "cuda");
                continue;
            }
            if (const char* eng = render_simd_iters_band(band.start, band_rows)) {
                notify(band_rows);
                add_layer(band.start, band.end, "fx64->fp64", eng);
            } else if (render_cuda_iters_band(band.start, band.end, false)) {
                add_layer(band.start, band.end, "fx64->fp64", "cuda");
            }
            continue;
        }

        // fp64 band
        if (render_cuda_iters_band(band.start, band.end, false)) {
            add_layer(band.start, band.end, "fp64", "cuda");
        } else if (const char* eng = render_simd_iters_band(band.start, band_rows)) {
            notify(band_rows);
            add_layer(band.start, band.end, "fp64", eng);
        }
    }

    return "fast(" + layers.str() + ")";
}

LnMapStats render_ln_map_hybrid(const LnMapParams& p, cv::Mat& out, const LnMapProgress& on_row_done) {
    if (!ln_map_variant_supported_by_simd(p.variant)) {
        return render_ln_map_openmp(p, out, on_row_done);
    }

    ensure_ln_out(p, out);
    const auto t0 = std::chrono::steady_clock::now();
    const LnCpuBackend cpu_backend = select_ln_cpu_backend(p);
    std::atomic<int> next_row{0};
    std::atomic<int> rows_done{0};
    std::atomic<int> gpu_rows{0};
    std::atomic<int> cpu_rows{0};
    std::atomic<bool> gpu_available{true};
    std::mutex progress_mu;
    const int h = p.height_t;
    const int gpu_batch = runtime_capabilities().cuda_low_end ? 16 : 48;
    const int cpu_batch = cpu_backend == LnCpuBackend::Openmp ? 2 : 4;

    auto notify = [&](int delta) {
        if (!on_row_done || delta <= 0) return;
        const int done = rows_done.fetch_add(delta, std::memory_order_relaxed) + delta;
        std::lock_guard<std::mutex> lock(progress_mu);
        on_row_done(std::min(done, h));
    };

    auto render_cpu = [&](int row0, int rows) {
        render_ln_cpu_rows_serial(p, out, row0, rows, cpu_backend);
        cpu_rows.fetch_add(rows, std::memory_order_relaxed);
        notify(rows);
    };

#if USE_CUDA_LN_MAP
    std::thread gpu_thread;
    if (fsd_cuda::cuda_ln_map_available()) {
        gpu_thread = std::thread([&]() {
            fsd_cuda::CudaLnMapParams cp = make_cuda_params(p);
            while (true) {
                const int row0 = next_row.fetch_add(gpu_batch, std::memory_order_relaxed);
                if (row0 >= h) break;
                const int rows = std::min(gpu_batch, h - row0);
                if (!gpu_available.load(std::memory_order_relaxed)) {
                    render_cpu(row0, rows);
                    continue;
                }
                try {
                    fsd_cuda::cuda_render_ln_map_rows(cp, out, row0, rows);
                    gpu_rows.fetch_add(rows, std::memory_order_relaxed);
                    notify(rows);
                } catch (...) {
                    gpu_available.store(false, std::memory_order_relaxed);
                    render_cpu(row0, rows);
                }
            }
        });
    } else {
        gpu_available.store(false, std::memory_order_relaxed);
    }
#else
    gpu_available.store(false, std::memory_order_relaxed);
#endif

    const int cpu_threads = default_render_threads();
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(cpu_threads));
    for (int i = 0; i < cpu_threads; ++i) {
        workers.emplace_back([&]() {
            while (true) {
                const int row0 = next_row.fetch_add(cpu_batch, std::memory_order_relaxed);
                if (row0 >= h) break;
                const int rows = std::min(cpu_batch, h - row0);
                render_cpu(row0, rows);
            }
        });
    }

    for (auto& worker : workers) worker.join();
#if USE_CUDA_LN_MAP
    if (gpu_thread.joinable()) gpu_thread.join();
#endif

    const auto t1 = std::chrono::steady_clock::now();
    LnMapStats stats;
    stats.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    stats.pixel_count = p.width_s * p.height_t;
    stats.scalar_used = "fp64";
    const int gpu = gpu_rows.load(std::memory_order_relaxed);
    const int cpu = cpu_rows.load(std::memory_order_relaxed);
    if (gpu > 0 && cpu > 0) {
        stats.engine_used = std::string("hybrid_cuda_") + ln_cpu_backend_name(cpu_backend);
    } else if (gpu > 0) {
        stats.engine_used = "cuda";
    } else {
        stats.engine_used = ln_cpu_backend_name(cpu_backend);
    }
    return stats;
}

LnMapStats render_ln_map_standard(const LnMapParams& p, cv::Mat& out, const LnMapProgress& on_row_done) {
    if (ln_map_scalar_is_fx64(p)) {
#if USE_CUDA_LN_MAP
        if (should_try_cuda_fx64(p)) {
            try {
                if (on_row_done) {
                    LnMapStats stats = render_ln_map_cuda_fx64_with_progress(p, out, on_row_done);
                    stats.precision_mode = "standard";
                    return stats;
                }
                const auto cudaStats = fsd_cuda::cuda_render_ln_map_fx64(make_cuda_params(p), out);
                LnMapStats stats;
                stats.elapsed_ms = cudaStats.elapsed_ms;
                stats.pixel_count = p.width_s * p.height_t;
                stats.engine_used = "cuda";
                stats.scalar_used = "fx64";
                stats.precision_mode = "standard";
                return stats;
            } catch (...) {
                if (p.engine == "cuda") throw;
            }
        }
#endif
        LnMapStats stats = render_ln_map_openmp_fx64(p, out, on_row_done);
        stats.precision_mode = "standard";
        return stats;
    }

    if (p.engine == "hybrid") {
        LnMapStats stats = render_ln_map_hybrid(p, out, on_row_done);
        stats.precision_mode = "standard";
        return stats;
    }

    if (should_try_cuda(p)) {
#if USE_CUDA_LN_MAP
        try {
            if (on_row_done) {
                LnMapStats stats = render_ln_map_cuda_with_progress(p, out, on_row_done);
                stats.precision_mode = "standard";
                return stats;
            }
            const auto cudaStats = fsd_cuda::cuda_render_ln_map(make_cuda_params(p), out);
            LnMapStats stats;
            stats.elapsed_ms = cudaStats.elapsed_ms;
            stats.pixel_count = p.width_s * p.height_t;
            stats.engine_used = "cuda";
            stats.scalar_used = "fp64";
            stats.precision_mode = "standard";
            return stats;
        } catch (...) {
            if (p.engine == "cuda") throw;
        }
#endif
    }

    if (should_try_avx512(p)) {
        try {
            LnMapStats stats = render_ln_map_avx512(p, out, on_row_done);
            stats.precision_mode = "standard";
            return stats;
        } catch (...) {
            if (p.engine == "avx512") throw;
        }
    }

    if (should_try_avx2(p)) {
        try {
            LnMapStats stats = render_ln_map_avx2(p, out, on_row_done);
            stats.precision_mode = "standard";
            return stats;
        } catch (...) {
            if (p.engine == "avx2") throw;
        }
    }

    LnMapStats stats = render_ln_map_openmp(p, out, on_row_done);
    stats.precision_mode = "standard";
    return stats;
}

LnMapStats render_ln_map_fast(const LnMapParams& p, cv::Mat& out, const LnMapProgress& on_row_done) {
    if (!ln_map_variant_supported_by_simd(p.variant)) {
        LnMapStats stats = render_ln_map_standard(p, out, on_row_done);
        stats.layer_summary = "fast_fallback=unsupported_variant";
        return stats;
    }

    ensure_ln_out(p, out);
    const auto t0 = std::chrono::steady_clock::now();
    const int h = p.height_t;
    const bool explicit_fp64 = p.scalar_type == "fp64";
    const bool requested_fx64 = ln_map_scalar_is_fx64(p);
    const bool cuda_fast =
#if USE_CUDA_LN_MAP
        engine_allows_cuda(p) && fsd_cuda::cuda_ln_map_available();
#else
        false;
#endif

    const LnCpuBackend fp32_backend = select_ln_cpu_fp32_backend(p);
    const bool cpu_fp32_available = fp32_backend == LnCpuBackend::Avx512 || fp32_backend == LnCpuBackend::Avx2;
    const LnCpuBackend fp64_backend = select_ln_cpu_backend(p);

    int fp32_end = rows_for_depth_octaves(p, p.fast_fp32_depth_octaves);
    int fp64_end = rows_for_depth_octaves(p, p.fast_fp64_depth_octaves);
    fp64_end = std::max(fp32_end, fp64_end);
    if (!cuda_fast && !cpu_fp32_available) fp32_end = 0;
    const bool use_deep_fx64 = (cuda_fast && !explicit_fp64) || requested_fx64;
    if (!use_deep_fx64) fp64_end = h;
    fp32_end = std::clamp(fp32_end, 0, h);
    fp64_end = std::clamp(fp64_end, fp32_end, h);

    std::atomic<int> rows_done{0};
    std::mutex progress_mu;
    auto notify = [&](int delta) {
        if (delta <= 0) return;
        const int done = rows_done.fetch_add(delta, std::memory_order_relaxed) + delta;
        if (on_row_done) {
            std::lock_guard<std::mutex> lock(progress_mu);
            on_row_done(std::min(done, h));
        }
    };

    const LnPlanScalar reference_scalar = requested_fx64 ? LnPlanScalar::Fx64 : LnPlanScalar::Fp64;
    std::string validation_summary;
    const std::vector<LnRenderBand> bands = make_fast_bands(
        p,
        fp32_end,
        fp64_end,
        use_deep_fx64,
        reference_scalar,
        validation_summary);

    std::ostringstream layers;
    bool first_layer = true;
    bool actual_fp32_layer = false;
    bool actual_fp64_layer = false;
    bool actual_fx64_layer = false;
    auto add_layer = [&](int start, int end, const char* scalar, const std::string& engine) {
        if (end <= start) return;
        if (!first_layer) layers << ";";
        first_layer = false;
        layers << scalar << "[" << start << "," << end << ")@" << engine;
    };

    for (const LnRenderBand& band : bands) {
        const int band_rows = band.end - band.start;
        if (band_rows <= 0) continue;

        if (band.scalar == LnPlanScalar::Fp32) {
            bool rendered = false;
#if USE_CUDA_LN_MAP
            if (cuda_fast) {
                try {
                    render_cuda_fp32_rows_with_progress(p, out, band.start, band_rows, notify);
                    add_layer(band.start, band.end, "fp32", "cuda");
                    actual_fp32_layer = true;
                    rendered = true;
                } catch (...) {
                    if (p.engine == "cuda") throw;
                }
            }
#endif
            if (!rendered && cpu_fp32_available) {
                render_layer_parallel(
                    band.start,
                    band_rows,
                    fp32_backend == LnCpuBackend::Avx512 ? 8 : 4,
                    [&](int row0, int rows) {
                        render_ln_cpu_fp32_rows_serial(p, out, row0, rows, fp32_backend);
                    },
                    notify);
                add_layer(band.start, band.end, "fp32", ln_cpu_backend_name(fp32_backend));
                actual_fp32_layer = true;
                rendered = true;
            }
            if (rendered) continue;
        }

        if (band.scalar == LnPlanScalar::Fx64) {
            bool rendered = false;
#if USE_CUDA_LN_MAP
            if (cuda_fast && !explicit_fp64) {
                try {
                    render_cuda_fx64_rows_with_progress(p, out, band.start, band_rows, notify);
                    add_layer(band.start, band.end, "fx64", "cuda");
                    actual_fx64_layer = true;
                    rendered = true;
                } catch (...) {
                    if (p.engine == "cuda") throw;
                }
            }
#endif
            if (!rendered && requested_fx64) {
                render_layer_parallel(
                    band.start,
                    band_rows,
                    2,
                    [&](int row0, int rows) {
                        render_ln_cpu_rows_serial(p, out, row0, rows, LnCpuBackend::Openmp);
                    },
                    notify);
                add_layer(band.start, band.end, "fx64", "openmp");
                actual_fx64_layer = true;
                rendered = true;
            }
            if (rendered) continue;
        }

        LnMapParams fp64p = p;
        fp64p.scalar_type = "fp64";
        render_layer_parallel(
            band.start,
            band_rows,
            fp64_backend == LnCpuBackend::Openmp ? 2 : 4,
            [&](int row0, int rows) {
                render_ln_cpu_rows_serial(fp64p, out, row0, rows, fp64_backend);
            },
            notify);
        add_layer(band.start, band.end, "fp64", ln_cpu_backend_name(fp64_backend));
        actual_fp64_layer = true;
    }

    const auto t1 = std::chrono::steady_clock::now();
    LnMapStats stats;
    stats.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    stats.pixel_count = p.width_s * p.height_t;
    stats.precision_mode = "fast";
    stats.layer_summary = layers.str();
    stats.validation_summary = validation_summary;
    stats.engine_used = stats.layer_summary.empty() ? "fast" : "fast(" + stats.layer_summary + ")";
    std::vector<std::string> scalars;
    if (actual_fp32_layer) scalars.emplace_back("fp32");
    if (actual_fp64_layer) scalars.emplace_back("fp64");
    if (actual_fx64_layer) scalars.emplace_back("fx64");
    if (scalars.empty()) {
        stats.scalar_used = "fp64";
    } else if (scalars.size() == 1) {
        stats.scalar_used = scalars.front();
    } else {
        std::ostringstream scalar_summary;
        scalar_summary << "layered(";
        for (size_t i = 0; i < scalars.size(); ++i) {
            if (i > 0) scalar_summary << ",";
            scalar_summary << scalars[i];
        }
        scalar_summary << ")";
        stats.scalar_used = scalar_summary.str();
    }
    return stats;
}

} // namespace

LnMapEqualization reconstructEqualization(
    double count_min, double period, double onset_cycles,
    bool colormap_wraps, Colormap colormap)
{
    LnMapEqualization eq;
    eq.count_min      = count_min;
    eq.period         = period;
    eq.onset_cycles   = onset_cycles;
    eq.colormap_wraps = colormap_wraps;
    eq.colormap       = colormap;
    eq.valid          = true;
    return eq;
}

static bool accumulate_ln_map_histograms_streamed(
    const LnMapParams& p,
    int max_rows,
    const LnMapProgress& on_row_done,
    LnEqHistogram& disk_hist,
    LnEqHistogram& all_hist
) {
    const int s = p.width_s;
    const int h = p.height_t;
    if (s <= 0 || h <= 0 || p.iterations <= 0) return false;

    const int chunk_rows = std::clamp(max_rows, 1, h);
    std::optional<TrigColumns> cols;
    auto get_cols = [&]() -> const TrigColumns& {
        if (!cols) cols = make_trig_columns(s);
        return *cols;
    };
    for (int row0 = 0; row0 < h; row0 += chunk_rows) {
        const int rows = std::min(chunk_rows, h - row0);
        LnMapParams chunk = p;
        chunk.height_t = rows;
        chunk.row_offset = p.row_offset + static_cast<double>(row0);
        chunk.equalization_override = nullptr;
        chunk.global_cdf_override = nullptr;

        if (try_accumulate_ln_map_equalization_histograms_cuda(chunk, disk_hist, all_hist)) {
            if (on_row_done) on_row_done(row0 + rows);
            continue;
        }

        std::vector<int> iters(static_cast<size_t>(s) * static_cast<size_t>(rows), p.iterations);
        const TrigColumns& chunk_cols = get_cols();
        auto report_chunk_progress = [&](int rows_done) {
            if (on_row_done) on_row_done(std::min(h, row0 + std::clamp(rows_done, 0, rows)));
        };

        if (lnmap_perturbation_applicable(chunk)) {
            compute_ln_perturb_iters(chunk, iters, report_chunk_progress);
        } else if (ln_map_fast_mode(chunk)) {
            compute_ln_field_fast(chunk, iters, chunk_cols, report_chunk_progress);
        } else {
            compute_ln_field(chunk, iters, chunk_cols, report_chunk_progress);
        }
        accumulate_ln_map_equalization_histograms(chunk, iters, chunk_cols, disk_hist, all_hist);
        if (on_row_done) on_row_done(row0 + rows);
    }
    return true;
}

LnMapEqualization build_ln_map_equalization_streamed(
    const LnMapParams& p, int max_rows, const LnMapProgress& on_row_done)
{
    LnEqHistogram disk_hist(p.iterations);
    LnEqHistogram all_hist(p.iterations);
    if (!accumulate_ln_map_histograms_streamed(p, max_rows, on_row_done, disk_hist, all_hist)) {
        return LnMapEqualization{};
    }

    const LnEqHistogram& selected = disk_hist.valid() ? disk_hist : all_hist;
    return finalize_ln_map_equalization_histogram(p, selected);
}

LnMapGlobalCdf build_ln_map_global_cdf_streamed(
    const LnMapParams& p, int max_rows, const LnMapProgress& on_row_done)
{
    LnEqHistogram disk_hist(p.iterations);
    LnEqHistogram all_hist(p.iterations);
    if (!accumulate_ln_map_histograms_streamed(p, max_rows, on_row_done, disk_hist, all_hist)) {
        return LnMapGlobalCdf{};
    }

    const LnEqHistogram& selected = disk_hist.valid() ? disk_hist : all_hist;
    return finalize_ln_map_global_cdf_histogram(selected);
}

LnMapEqualization build_map_equalization(
    const MapParams& p, const FieldOutput& field, bool distance_weighted, double cpo)
{
    if (field.metric != Metric::Escape || p.width <= 0 || p.height <= 0 || p.iterations <= 0)
        return LnMapEqualization{};
    if (field.iter_u32.size() != static_cast<size_t>(p.width) * static_cast<size_t>(p.height))
        return LnMapEqualization{};
    if (!(cpo > 0.0) || !std::isfinite(cpo)) cpo = 1.0;

    // Cartesian viewport geometry (matches finalFrameAbs2At / render_map sampling): scale is
    // the imaginary-axis span; pixel centers at (x+0.5, y+0.5).
    const double aspect = static_cast<double>(p.width) / static_cast<double>(p.height);
    const double spanIm = p.scale;
    const double spanRe = p.scale * aspect;
    const double reMin  = p.center_re - spanRe * 0.5;
    const double imMax  = p.center_im + spanIm * 0.5;
    // Distance-from-center floor: clamp ρ so the central ~quarter-diameter region dominates
    // uniformly instead of a single pixel blowing up the 1/ρ² weight (stability for live zoom).
    const double rho_floor = std::max(1e-300, p.scale / 8.0);
    const double rho_floor_sq = rho_floor * rho_floor;

    std::vector<double> hist(static_cast<size_t>(p.iterations), 0.0);
    double total = 0.0;
    for (int y = 0; y < p.height; ++y) {
        const double im = imMax - (static_cast<double>(y) + 0.5) / static_cast<double>(p.height) * spanIm;
        const size_t row_offset = static_cast<size_t>(y) * static_cast<size_t>(p.width);
        for (int x = 0; x < p.width; ++x) {
            const double re = reMin + (static_cast<double>(x) + 0.5) / static_cast<double>(p.width) * spanRe;
            double weight = 1.0;
            if (distance_weighted) {
                if (re * re + im * im > 4.0) continue;  // origin disk |c|<=2 only
                const double dre = re - p.center_re, dim = im - p.center_im;
                weight = 1.0 / std::max(dre * dre + dim * dim, rho_floor_sq);
            }
            const int it = static_cast<int>(field.iter_u32[row_offset + static_cast<size_t>(x)]);
            if (it >= 0 && it < p.iterations) {
                hist[static_cast<size_t>(it)] += weight;
                total += weight;
            }
        }
    }
    if (total <= 0.0) return LnMapEqualization{};

    // Period tracks zoom depth, not per-frame pixel extremes (which diverge as ρ→0 at the
    // target): ~1 palette cycle per octave of magnification beyond the |c|<=2 disk (height 4).
    const double total_octaves = std::max(1.0, std::log2(4.0 / std::max(1e-300, p.scale)));
    return finalize_equalization(hist, total, total_octaves, cpo,
                                 p.colormap, ln_map_colormap_wraps_phase(p.colormap));
}

void colorize_map_field_equalized(
    const MapParams& p, const FieldOutput& field,
    const LnMapEqualization& eq, cv::Mat& out)
{
    const size_t expected = static_cast<size_t>(p.width) * static_cast<size_t>(p.height);
    if (field.metric != Metric::Escape || field.iter_u32.size() != expected) return;
    if (out.rows != p.height || out.cols != p.width || out.type() != CV_8UC3)
        out.create(p.height, p.width, CV_8UC3);

    for (int y = 0; y < p.height; ++y) {
        uint8_t* row = out.ptr<uint8_t>(y);
        const size_t row_offset = static_cast<size_t>(y) * static_cast<size_t>(p.width);
        for (int x = 0; x < p.width; ++x) {
            const int it = static_cast<int>(field.iter_u32[row_offset + static_cast<size_t>(x)]);
            uint8_t* px = row + 3 * x;
            if (it >= p.iterations) { px[0] = px[1] = px[2] = 255; continue; }  // interior → white
            eq.colorize(it, px[0], px[1], px[2]);
        }
    }
}

void LnMapEqualization::colorize(int it, uint8_t& b, uint8_t& g, uint8_t& r) const {
    // Absolute phase in palette cycles: shallowest count → 0, growing ~1 cycle per
    // octave of zoom (for cyclesPerOctave == 1).
    double phase = (static_cast<double>(it) - count_min) / period;
    if (phase < 0.0) phase = 0.0;

    // One-time black→start-color lead-in: the first 1/6 cycle (the shallowest counts,
    // i.e. the opening of the zoom) fades pure black → the palette start color. Hue is
    // held at the cycle start while brightness ramps, flowing into the wheel at 1/6.
    if (phase < onset_cycles) {
        double v = std::clamp(phase / onset_cycles, 0.0, 1.0);
        v = v * v * (3.0 - 2.0 * v);  // smoothstep
        uint8_t sb = 0, sg = 0, sr = 0;
        colorize_field_bgr(0.0, colormap, sb, sg, sr);  // cycle-start color (green for spectral1530)
        b = static_cast<uint8_t>(clamp255(static_cast<int>(std::lround(static_cast<double>(sb) * v))));
        g = static_cast<uint8_t>(clamp255(static_cast<int>(std::lround(static_cast<double>(sg) * v))));
        r = static_cast<uint8_t>(clamp255(static_cast<int>(std::lround(static_cast<double>(sr) * v))));
        return;
    }

    // Periodic wheel, anchored so frac==0 (cycle start) lands exactly at onset_cycles.
    const double wheel = phase - onset_cycles;
    double f = wheel - std::floor(wheel);
    if (!colormap_wraps) f = 1.0 - std::abs(2.0 * f - 1.0);  // triangle reflect → seam-free
    colorize_field_bgr(f, colormap, b, g, r);
}

bool ln_map_variant_supported_by_simd(Variant v) {
    const int id = static_cast<int>(v);
    return id >= 0 && id <= 9;
}

bool ln_map_color_mode_supported(const std::string& mode) noexcept {
    return mode == "escape" ||
           mode == "hist_eq" ||
           mode == "row_eq" ||
           mode == "log_lift" ||
           mode == "bands" ||
           mode == "frontier";
}

LnMapStats render_ln_map_openmp_rows(const LnMapParams& p, cv::Mat& out, int row_start, int row_count, const LnMapProgress& on_row_done) {
    ensure_ln_out(p, out);
    const auto [start, end] = clamp_rows(p, row_start, row_count);
    const auto t0 = std::chrono::steady_clock::now();
    const TrigColumns cols = make_trig_columns(p.width_s);
    dispatch_openmp_rows_impl(p, out, start, end, cols, true, on_row_done);
    const auto t1 = std::chrono::steady_clock::now();
    LnMapStats stats;
    stats.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    stats.pixel_count = p.width_s * (end - start);
    stats.engine_used = "openmp";
    stats.scalar_used = "fp64";
    stats.precision_mode = p.precision_mode;
    return stats;
}

LnMapStats render_ln_map_openmp(const LnMapParams& p, cv::Mat& out, const LnMapProgress& on_row_done) {
    return render_ln_map_openmp_rows(p, out, 0, p.height_t, on_row_done);
}

// ---------------------------------------------------------------------------
// Perturbation-theory LnMap renderer for deep-zoom Mandelbrot.
// Uses the same reference-orbit + delta iteration as the cartesian perturbation
// renderer, but maps pixels in log-polar coordinates:
//   delta_c = r_mag * (cos(theta), sin(theta))
// where r_mag = exp(ln4 - row * 2π/s).
// ---------------------------------------------------------------------------

// Perturbation serves every color mode: escape colorizes directly, the
// mapped modes (hist_eq / row_eq / log_lift / bands / frontier) consume the
// raw iteration field. Gate on the innermost strip radius — above ~1e-13 the
// plain fp64 SIMD paths are exact and faster.
static double lnmap_innermost_radius(const LnMapParams& p) {
    const double global_bottom = p.row_offset + static_cast<double>(p.height_t - 1);
    const double k_min = LN_FOUR - global_bottom * TAU
                                   / static_cast<double>(p.width_s);
    return std::exp(std::max(k_min, -690.0));  // keep > 0 for log2
}

static bool lnmap_perturbation_applicable(const LnMapParams& p) {
    if (p.variant != Variant::Mandelbrot) return false;
    if (p.center_re_str.empty())          return false;
    return lnmap_innermost_radius(p) < 1e-13;
}

// Perturbation iteration for the whole strip. Calls sink(idx, iter) once per
// pixel (from parallel regions, distinct pixels only) and returns the engine
// actually used ("cuda" / "avx512" / "avx2" / "openmp").
template <typename Sink>
static std::string run_ln_perturbation_field(const LnMapParams& p,
                                             const LnMapProgress& on_row_done,
                                             Sink&& sink)
{
    const int S       = p.width_s;
    const int T       = p.height_t;
    const double bail2 = p.bailout_sq;
    const int max_iter = p.iterations;
    const bool is_julia = p.julia;
    const int thread_count = default_render_threads();

    // The strip spans radii from LN_FOUR down to the innermost row; the
    // innermost radius drives the reference-precision tier (fp128 vs MPFR).
    const double r_min = lnmap_innermost_radius(p);

    // Primary reference orbit at the viewport center; for Julia additionally
    // the critical orbit of c_julia as the rebase target (see perturbation.hpp).
    RefOrbit ref;
    RefOrbit crit;
    if (is_julia) {
        ref = compute_reference_orbit_julia_auto(
            p.center_re_str, p.center_im_str,
            p.julia_re, p.julia_im, max_iter, bail2, r_min);
        // The critical orbit is the long-term rebase target at the same
        // depth, so it needs the same iteration-precision tier as the
        // primary (c_julia itself is exact in every tier).
        crit = compute_reference_orbit_scaled(p.julia_re, p.julia_im,
                                              max_iter, bail2, r_min);
    } else {
        ref = compute_reference_orbit_auto(
            p.center_re_str, p.center_im_str, max_iter, bail2, r_min);
    }
    const RefOrbit& kref = is_julia ? crit : ref;

    const double* Rr = ref.z_re.data();
    const double* Ri = ref.z_im.data();
    const int     Rlen = static_cast<int>(ref.z_re.size());
    const double* Kr = kref.z_re.data();
    const double* Ki = kref.z_im.data();
    const int     Klen = static_cast<int>(kref.z_re.size());

    // Combined orbit table for the batch kernels: R then K (Mandelbrot
    // aliases both windows onto the same orbit; see perturbation.hpp).
    std::vector<double> tab_re_store, tab_im_store;
    const double* tab_re = Rr;
    const double* tab_im = Ri;
    int tab_len = Rlen;
    int k_off = 0;
    if (is_julia) {
        tab_re_store.assign(ref.z_re.begin(), ref.z_re.end());
        tab_re_store.insert(tab_re_store.end(), kref.z_re.begin(), kref.z_re.end());
        tab_im_store.assign(ref.z_im.begin(), ref.z_im.end());
        tab_im_store.insert(tab_im_store.end(), kref.z_im.begin(), kref.z_im.end());
        tab_re = tab_re_store.data();
        tab_im = tab_im_store.data();
        tab_len = Rlen + Klen;
        k_off = Rlen;
    }
    int start_off = 0, start_len = Rlen;
    double dz_shift_re = 0.0, dz_shift_im = 0.0;
    if (Rlen < 2) {
        dz_shift_re = Rr[0];
        dz_shift_im = Ri[0];
        start_off = k_off;
        start_len = Klen;
    }
    const bool batch_ok = start_len >= 2 && Klen >= 2;

    // Explicit engine request, or auto → AVX-512, CUDA, AVX2, scalar
    // (matching select_map_engine's no-benchmark preference order).
    std::string want = p.engine;
    if (want == "auto") {
        if (perturb_avx512_available()) want = "avx512";
#if USE_CUDA
        else if (fsd_cuda::cuda_available()) want = "cuda";
#endif
        else if (perturb_avx2_available()) want = "avx2";
        else want = "openmp";
    }
    [[maybe_unused]] const bool wants_cuda = want == "cuda" || want == "hybrid";
    using PerturbBatchFn = void (*)(
        const double*, const double*, int, int, int, int,
        const double*, const double*, const double*, const double*,
        int, int, double, int32_t*, double*) noexcept;
    // Best CPU batch kernel — the primary path for the AVX engines and the
    // fallback when a CUDA render fails.
    PerturbBatchFn batch = nullptr;
    const char* batch_engine = "openmp";
    if (batch_ok && want != "openmp") {
        if (want != "avx2" && perturb_avx512_available()) {
            batch = perturb_iterate_batch_avx512;
            batch_engine = "avx512";
        } else if (perturb_avx2_available()) {
            batch = perturb_iterate_batch_avx2;
            batch_engine = "avx2";
        }
    }
    std::string engine_used = "openmp";
    bool rendered = false;

#if USE_CUDA
    if (!rendered && batch_ok && wants_cuda && fsd_cuda::cuda_available()) {
        // Row-chunked (rather than one whole-field launch) so a solid-white
        // chunk can short-circuit every deeper chunk in the same strip — same
        // rationale as the CPU path's first_interior_row check below. Chunk
        // size trades early-exit granularity against reference-table
        // (tab_re/tab_im) re-upload overhead paid on every launch.
        constexpr int kCudaPerturbChunkRows = 512;
        const double base_ln_r0 = LN_FOUR - p.row_offset * TAU / static_cast<double>(S);
        const double k_step = TAU / static_cast<double>(S);
        const int chunk_cap = std::min(T, kCudaPerturbChunkRows);
        std::vector<uint32_t> iters(static_cast<size_t>(S) * static_cast<size_t>(chunk_cap));
        std::vector<float> norms(static_cast<size_t>(S) * static_cast<size_t>(chunk_cap));
        bool cuda_ok = true;
        int rows_filled = 0;
        for (int row0 = 0; row0 < T; row0 += kCudaPerturbChunkRows) {
            const int rows = std::min(kCudaPerturbChunkRows, T - row0);
            fsd_cuda::CudaPerturbParams cp;
            cp.width = S; cp.height = rows; cp.iterations = max_iter;
            cp.bailout_sq = bail2;
            cp.offset_mode = 1;
            cp.ln_r0 = base_ln_r0 - static_cast<double>(row0) * k_step;
            cp.k_step = k_step;
            cp.theta_step = TAU / static_cast<double>(S);
            cp.julia = is_julia;
            cp.dz_shift_re = dz_shift_re; cp.dz_shift_im = dz_shift_im;
            cp.tab_re = tab_re; cp.tab_im = tab_im; cp.tab_len = tab_len;
            cp.start_off = start_off; cp.start_len = start_len;
            cp.k_off = k_off; cp.k_len = Klen;
            if (!fsd_cuda::cuda_render_perturb_field(cp, iters.data(), norms.data(), nullptr)) {
                cuda_ok = false;
                break;
            }
            #pragma omp parallel for num_threads(thread_count) schedule(static)
            for (int local_row = 0; local_row < rows; ++local_row) {
                const size_t row_base = static_cast<size_t>(row0 + local_row) * S;
                const size_t src_base = static_cast<size_t>(local_row) * S;
                for (int x = 0; x < S; ++x) {
                    sink(row_base + x, static_cast<int>(iters[src_base + static_cast<size_t>(x)]));
                }
            }
            rows_filled = row0 + rows;
            if (on_row_done) on_row_done(rows_filled);

            bool last_row_all_interior = true;
            const size_t last_row_base = static_cast<size_t>(rows - 1) * S;
            for (int x = 0; x < S; ++x) {
                if (iters[last_row_base + static_cast<size_t>(x)] < static_cast<uint32_t>(max_iter)) {
                    last_row_all_interior = false;
                    break;
                }
            }
            if (last_row_all_interior && rows_filled < T) {
                // Deepest row of this chunk proved solid-white: every remaining,
                // deeper row is guaranteed solid-white too (see the CPU path's
                // identical argument) — fill it directly, no further launches.
                #pragma omp parallel for num_threads(thread_count) schedule(static)
                for (int row = rows_filled; row < T; ++row) {
                    const size_t row_base = static_cast<size_t>(row) * S;
                    for (int x = 0; x < S; ++x) sink(row_base + x, max_iter);
                }
                if (on_row_done) on_row_done(T);
                rows_filled = T;
                break;
            }
        }
        if (cuda_ok) {
            engine_used = "cuda";
            rendered = true;
        }
    }
#endif

    if (!rendered) {
        const TrigColumns cols = make_trig_columns(S);
        std::atomic<int> rows_done{0};
        // Set once a whole row (every column — a full ring at constant radius
        // around the same center) comes back non-escaping: an interior point
        // has an open neighbourhood inside the set, so every smaller radius —
        // every row with a larger index — is guaranteed interior too. Rows
        // already claimed by a thread when this fires just finish normally
        // (bounded overshoot under dynamic scheduling); every row grabbed
        // after it is filled directly, with no reference-orbit iteration.
        std::atomic<int> first_interior_row{T};

        #pragma omp parallel num_threads(thread_count)
        {
            std::vector<double> bdz_re, bdz_im, bdc_re, bdc_im;
            std::vector<int32_t> b_iter;
            std::vector<double> b_mag2;
            if (batch) {
                bdz_re.resize(static_cast<size_t>(S));
                bdz_im.resize(static_cast<size_t>(S));
                bdc_re.resize(static_cast<size_t>(S));
                bdc_im.resize(static_cast<size_t>(S));
                b_iter.resize(static_cast<size_t>(S));
                b_mag2.resize(static_cast<size_t>(S));
            }

            #pragma omp for schedule(dynamic, 8)
            for (int row = 0; row < T; ++row) {
                const size_t row_base = static_cast<size_t>(row) * S;

                if (row >= first_interior_row.load(std::memory_order_relaxed)) {
                    for (int x = 0; x < S; ++x) sink(row_base + x, max_iter);
                } else {
                    const double global_row = p.row_offset + static_cast<double>(row);
                    const double k = LN_FOUR - global_row * TAU / static_cast<double>(S);
                    const double r_mag = std::exp(k);
                    bool row_all_interior = true;

                    if (batch) {
                        for (int x = 0; x < S; ++x) {
                            const double off_re = r_mag * cols.cos_col[static_cast<size_t>(x)];
                            const double off_im = r_mag * cols.sin_col[static_cast<size_t>(x)];
                            if (is_julia) {
                                bdz_re[static_cast<size_t>(x)] = off_re + dz_shift_re;
                                bdz_im[static_cast<size_t>(x)] = off_im + dz_shift_im;
                                bdc_re[static_cast<size_t>(x)] = 0.0;
                                bdc_im[static_cast<size_t>(x)] = 0.0;
                            } else {
                                bdz_re[static_cast<size_t>(x)] = dz_shift_re;
                                bdz_im[static_cast<size_t>(x)] = dz_shift_im;
                                bdc_re[static_cast<size_t>(x)] = off_re;
                                bdc_im[static_cast<size_t>(x)] = off_im;
                            }
                        }
                        batch(tab_re, tab_im, start_off, start_len, k_off, Klen,
                              bdz_re.data(), bdz_im.data(), bdc_re.data(), bdc_im.data(),
                              S, max_iter, bail2, b_iter.data(), b_mag2.data());
                        for (int x = 0; x < S; ++x) {
                            const int32_t it = b_iter[static_cast<size_t>(x)];
                            if (it < max_iter) row_all_interior = false;
                            sink(row_base + x, it);
                        }
                    } else {
                        for (int x = 0; x < S; ++x) {
                            const double off_re = r_mag * cols.cos_col[static_cast<size_t>(x)];
                            const double off_im = r_mag * cols.sin_col[static_cast<size_t>(x)];

                            const double dz0_re = is_julia ? off_re : 0.0;
                            const double dz0_im = is_julia ? off_im : 0.0;
                            const double dc_re  = is_julia ? 0.0 : off_re;
                            const double dc_im  = is_julia ? 0.0 : off_im;

                            const PerturbPixel res = perturb_iterate(
                                Rr, Ri, Rlen, Kr, Ki, Klen,
                                dz0_re, dz0_im, dc_re, dc_im, max_iter, bail2);

                            if (res.iter < max_iter) row_all_interior = false;
                            sink(row_base + x, res.iter);
                        }
                    }

                    if (row_all_interior) {
                        int expected = first_interior_row.load(std::memory_order_relaxed);
                        while (row < expected &&
                               !first_interior_row.compare_exchange_weak(
                                   expected, row, std::memory_order_relaxed)) {
                        }
                    }
                }
                if (on_row_done) {
                    const int done = rows_done.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (done == T || (done % 16) == 0) on_row_done(done);
                }
            }
        }
        engine_used = batch_engine;
    }

    return engine_used;
}

// Escape color mode: colorize straight from the iteration counts.
static LnMapStats render_ln_map_perturbation(const LnMapParams& p, cv::Mat& out,
                                              const LnMapProgress& on_row_done)
{
    ensure_ln_out(p, out);
    const auto t0 = std::chrono::steady_clock::now();
    const int S = p.width_s;
    const int max_iter = p.iterations;

    const std::string engine_used = run_ln_perturbation_field(
        p, on_row_done,
        [&](size_t idx, int iter) {
            uint8_t* px = out.ptr<uint8_t>(static_cast<int>(idx / S))
                        + 3 * static_cast<int>(idx % S);
            colorize_escape_bgr(iter, max_iter, p.colormap,
                                0.0, false, px[0], px[1], px[2]);
        });

    const auto t1 = std::chrono::steady_clock::now();
    LnMapStats stats;
    stats.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    stats.pixel_count = p.width_s * p.height_t;
    stats.engine_used = engine_used;
    stats.scalar_used = "perturbation";
    stats.precision_mode = "standard";
    return stats;
}

// Raw iteration field for the mapped color modes (hist_eq/global CDF, row_eq,
// log_lift, bands, frontier) — declared above render_ln_map_mapped.
static std::string compute_ln_perturb_iters(const LnMapParams& p,
                                            std::vector<int>& iters,
                                            const LnMapProgress& on_row_done)
{
    return "perturbation_" + run_ln_perturbation_field(
        p, on_row_done,
        [&](size_t idx, int iter) { iters[idx] = iter; });
}

LnMapStats render_ln_map(const LnMapParams& p, cv::Mat& out, const LnMapProgress& on_row_done) {
    if (!ln_map_color_mode_supported(p.color_mode)) {
        throw std::runtime_error("invalid ln-map color mode");
    }
    if (p.color_mode != "escape") {
        // Mapped modes route deep strips through perturbation internally.
        return render_ln_map_mapped(p, out, on_row_done);
    }
    if (lnmap_perturbation_applicable(p)) {
        return render_ln_map_perturbation(p, out, on_row_done);
    }
    if (ln_map_fast_mode(p)) {
        return render_ln_map_fast(p, out, on_row_done);
    }
    return render_ln_map_standard(p, out, on_row_done);
}

} // namespace fsd::compute
