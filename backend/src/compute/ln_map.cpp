// compute/ln_map.cpp

#include "ln_map.hpp"

#include "engine_select.hpp"
#include "escape_time.hpp"
#include "fx64_raw.hpp"
#include "map_kernel_avx2.hpp"
#include "map_kernel_avx512.hpp"
#include "parallel.hpp"

#if defined(HAS_CUDA_KERNEL)
#  include "cuda/ln_map.cuh"
#  define USE_CUDA_LN_MAP 1
#else
#  define USE_CUDA_LN_MAP 0
#endif

#include <opencv2/core.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace fsd::compute {
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
           colormap == Colormap::Twilight;
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
        const double k = LN_FOUR - static_cast<double>(row) * TAU / static_cast<double>(s);
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
        const double k = LN_FOUR - static_cast<double>(row) * TAU / static_cast<double>(s);
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

    return {elapsed_ms, p.width_s * p.height_t, "cuda", "fp64", p.precision_mode, "", ""};
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
    return {elapsed_ms, p.width_s * p.height_t, "cuda", "fx64", p.precision_mode, "", ""};
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
    const double k = LN_FOUR - static_cast<double>(row) * TAU / static_cast<double>(p.width_s);
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
    const float k = ln_four - static_cast<float>(row) * tau / width_s;
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
    const double k = LN_FOUR - static_cast<double>(row) * TAU / static_cast<double>(p.width_s);
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
        const double k = LN_FOUR - static_cast<double>(row) * TAU / static_cast<double>(s);
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

LnMapStats render_ln_map_mapped(const LnMapParams& p, cv::Mat& out, const LnMapProgress& on_row_done) {
    ensure_ln_out(p, out);
    const auto t0 = std::chrono::steady_clock::now();
    const int s = p.width_s;
    const int h = p.height_t;
    const size_t pixel_count = static_cast<size_t>(s) * static_cast<size_t>(h);
    const std::string mode = p.color_mode;
    const TrigColumns cols = make_trig_columns(s);
    std::vector<int> iters(pixel_count, p.iterations);

    compute_ln_iters_fp64(p, iters, cols, on_row_done);

    const bool needs_global_cdf = mode == "hist_eq" || mode == "bands" || mode == "frontier";
    std::vector<unsigned long long> hist;
    unsigned long long total = 0;
    int first_hist_iter = p.iterations;
    if (needs_global_cdf) {
        hist.assign(static_cast<size_t>(p.iterations), 0ULL);
        for (int row = 0; row < h; ++row) {
            const double k = LN_FOUR - static_cast<double>(row) * TAU / static_cast<double>(s);
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
                const double q = blended_global_q_for_iter(it);
                const double palette_window = 0.58 + 0.42 * depth01;
                const double palette_floor = 0.10 + 0.04 * (1.0 - depth01);
                mapped = palette_floor + q * std::max(0.0, palette_window - palette_floor);
                mapped = apply_ln_map_depth_phase(mapped, 0.22 * depth01, p.colormap);
            }
            colorize_field_bgr(mapped, p.colormap, px[0], px[1], px[2]);
        }
    }

    const auto t1 = std::chrono::steady_clock::now();
    LnMapStats stats;
    stats.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    stats.pixel_count = p.width_s * p.height_t;
    stats.engine_used = mode == "hist_eq" ? "openmp_hist_eq" : "openmp_" + mode;
    stats.scalar_used = "fp64";
    stats.precision_mode = p.precision_mode;
    std::ostringstream layer;
    layer << "color=" << mode << ",histDomain=|c|<=2";
    if (needs_global_cdf) layer << ",histPixels=" << total;
    if (needs_global_cdf) layer << ",underflowTail=raw";
    if (mode == "row_eq") layer << ",rowLocal=1";
    if (mode == "log_lift") layer << ",curve=log1p64";
    if (mode == "bands") layer << ",bands=18+72";
    if (mode == "frontier") layer << ",edge=gradient";
    stats.layer_summary = layer.str();
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
                const auto stats = fsd_cuda::cuda_render_ln_map_fx64(make_cuda_params(p), out);
                return {stats.elapsed_ms, p.width_s * p.height_t, "cuda", "fx64", "standard", "", ""};
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
            const auto stats = fsd_cuda::cuda_render_ln_map(make_cuda_params(p), out);
            return {stats.elapsed_ms, p.width_s * p.height_t, "cuda", "fp64", "standard", "", ""};
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

LnMapStats render_ln_map(const LnMapParams& p, cv::Mat& out, const LnMapProgress& on_row_done) {
    if (!ln_map_color_mode_supported(p.color_mode)) {
        throw std::runtime_error("invalid ln-map color mode");
    }
    if (p.color_mode != "escape") {
        return render_ln_map_mapped(p, out, on_row_done);
    }
    if (ln_map_fast_mode(p)) {
        return render_ln_map_fast(p, out, on_row_done);
    }
    return render_ln_map_standard(p, out, on_row_done);
}

} // namespace fsd::compute
