// compute/tile_scheduler.cpp
//
// Hybrid CPU+GPU tile scheduler implementation.

#include "tile_scheduler.hpp"
#include "engine_select.hpp"
#include "fx64_raw.hpp"
#include "map_kernel.hpp"
#include "map_kernel_avx2.hpp"
#include "map_kernel_avx512.hpp"
#include "parallel.hpp"

#if defined(HAS_CUDA_KERNEL)
#  include "cuda/map_kernel.cuh"
#  define USE_CUDA 1
#else
#  define USE_CUDA 0
#endif

#include <opencv2/core.hpp>
#ifdef _OPENMP
#  include <omp.h>
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fsd::compute {

// ---- Tile descriptor ----

struct Tile {
    int x, y;     // top-left pixel in the full viewport
    int w, h;     // tile dimensions (may be smaller at right/bottom edges)
};

static std::vector<Tile> make_tiles(int W, int H, int tile_size) {
    std::vector<Tile> tiles;
    for (int ty = 0; ty < H; ty += tile_size) {
        for (int tx = 0; tx < W; tx += tile_size) {
            Tile t;
            t.x = tx;
            t.y = ty;
            t.w = std::min(tile_size, W - tx);
            t.h = std::min(tile_size, H - ty);
            tiles.push_back(t);
        }
    }
    return tiles;
}

static bool supports_fx64_int_path(const MapParams& p) {
    const int variant_id = static_cast<int>(p.variant);
    return variant_id >= 0
        && variant_id <= 9
        && p.metric != Metric::MinPairwiseDist
        && !p.smooth;
}

static bool requested_q459(const MapParams& p) {
    const std::string scalar = map_effective_scalar_type(p);
    return scalar == "q4.59" || scalar == "q459" ||
           scalar == "fx59" || scalar == "fixed59";
}

static bool requested_q360(const MapParams& p) {
    const std::string scalar = map_effective_scalar_type(p);
    return scalar == "q3.60" || scalar == "q360" ||
           scalar == "fx60" || scalar == "fixed60";
}

static bool requested_fixed(const MapParams& p) {
    const std::string scalar = map_effective_scalar_type(p);
    return scalar == "q3.60" || scalar == "q360" ||
           scalar == "fx60" || scalar == "fixed60" ||
           scalar == "q4.59" || scalar == "q459" ||
           scalar == "fx59" || scalar == "fixed59" ||
           scalar == "fx64" || scalar == "q6.57" ||
           scalar == "q657" || scalar == "fixed57";
}

static bool is_cancelled_exception(const std::exception& ex) {
    return std::string(ex.what()) == "cancelled";
}

// Convert calibrated aggregate CPU/GPU costs into the number of tiles the
// single GPU worker reserves per atomic queue claim. CPU profile timings are
// measured with the normal CPU pool, hence multiplying by `cpu_workers`
// translates the aggregate rate ratio into this queue's one-tile-per-worker
// claim granularity. A strict clamp limits cancellation latency and prevents a
// noisy calibration from monopolising an arbitrarily large render.
static size_t calibrated_gpu_batch(
    const MapParams& p,
    int tile_size,
    const std::string& scalar,
    const std::string& cpu_engine,
    int cpu_workers,
    size_t tile_count,
    size_t fallback)
{
    const size_t max_batch = std::max<size_t>(
        1u, std::min<size_t>(64u, tile_count > 1 ? tile_count - 1 : 1u));
    const auto clamp_batch = [&](size_t batch) {
        return std::clamp<size_t>(batch, 1u, max_batch);
    };
    const double tile_work =
        static_cast<double>(std::max(1, tile_size)) *
        static_cast<double>(std::max(1, tile_size)) *
        static_cast<double>(std::max(1, p.iterations));
    const std::vector<BenchmarkEntry> profile = benchmark_cache_snapshot();
    const auto cpu_ms = predict_profiled_elapsed_ms(
        profile, "map_field", scalar, cpu_engine, tile_work);
    const auto gpu_ms = predict_profiled_elapsed_ms(
        profile, "map_field", scalar, "cuda", tile_work);
    if (!cpu_ms.has_value() || !gpu_ms.has_value() ||
        !std::isfinite(*cpu_ms) || !std::isfinite(*gpu_ms) ||
        !(*cpu_ms > 0.0) || !(*gpu_ms > 0.0)) {
        return clamp_batch(fallback);
    }

    const double gpu_to_cpu_rate = *cpu_ms / *gpu_ms;
    const double desired_per_cpu_wave =
        static_cast<double>(std::max(1, cpu_workers)) * gpu_to_cpu_rate;
    const double desired_total_share = static_cast<double>(std::max<size_t>(1u, tile_count)) *
        gpu_to_cpu_rate / (1.0 + gpu_to_cpu_rate);
    const double desired = std::min(desired_per_cpu_wave, desired_total_share);
    if (!std::isfinite(desired)) return clamp_batch(fallback);
    return clamp_batch(static_cast<size_t>(std::max<long long>(1LL, std::llround(desired))));
}

// ---- Tile render helpers ----

// Render a single tile into the output mat using the best available CPU path.
static double render_tile_cpu(
    const MapParams& base, const Tile& t,
    cv::Mat& out, bool use_avx2, bool use_avx512, bool use_fx,
    const std::string& fixed_scalar_type
) {
    // Build a MapParams for this tile.
    MapParams p = base;
    const double aspect  = map_viewport_aspect(base);
    const double span_im = base.scale;
    const double span_re = base.scale * aspect;
    const double re_step = span_re / base.width;
    const double im_step = span_im / base.height;

    const double tile_dx = (static_cast<double>(t.x) + t.w * 0.5 -
                            base.width * 0.5) * re_step;
    const double tile_dy = -(static_cast<double>(t.y) + t.h * 0.5 -
                             base.height * 0.5) * im_step;
    const double rotation_rad = base.rotation_deg * M_PI / 180.0;
    const double cos_rotation = std::cos(rotation_rad);
    const double sin_rotation = std::sin(rotation_rad);

    p.center_re = base.center_re + tile_dx * cos_rotation - tile_dy * sin_rotation;
    p.center_im = base.center_im + tile_dx * sin_rotation + tile_dy * cos_rotation;
    p.scale     = t.h * im_step;    // height of this tile in complex units
    p.viewport_aspect = (t.w * re_step) / (t.h * im_step);
    p.width     = t.w;
    p.height    = t.h;
    p.engine    = use_avx512 ? "avx512" : (use_avx2 ? "avx2" : "openmp");
    p.scalar_type = use_fx ? fixed_scalar_type : "fp64";
    p.render_threads = 1;

    // Create a submat view for this tile.
    cv::Mat tile_mat = out(cv::Rect(t.x, t.y, t.w, t.h));

    MapStats stats;
    if (use_avx512 && !use_fx) {
        stats = render_map_avx512_fp64(p, tile_mat);
    } else if (use_avx2 && !use_fx) {
        stats = render_map_avx2_fp64(p, tile_mat);
    } else {
        // OpenMP path (single-threaded for this tile since the outer scheduler
        // calls render_tile_cpu from multiple CPU workers concurrently).
        stats = render_map(p, tile_mat);
    }
    return stats.elapsed_ms;
}

#if USE_CUDA
template <int FRAC>
static void assign_cuda_fixed_viewport(
    fsd_cuda::CudaMapParams& cp,
    const FixedViewportRaw<FRAC>& vp
) {
    cp.fx64_viewport.center_re_raw = vp.center_re_raw;
    cp.fx64_viewport.center_im_raw = vp.center_im_raw;
    cp.fx64_viewport.span_re_raw = vp.span_re_raw;
    cp.fx64_viewport.span_im_raw = vp.span_im_raw;
    cp.fx64_viewport.width = vp.width;
    cp.fx64_viewport.height = vp.height;
    cp.fx64_viewport.first_re_raw = vp.first_re_raw;
    cp.fx64_viewport.first_im_raw = vp.first_im_raw;
    cp.fx64_viewport.step_re_raw = vp.step_re_raw;
    cp.fx64_viewport.step_im_raw = vp.step_im_raw;
    cp.fx64_viewport.julia_re_raw = vp.julia_re_raw;
    cp.fx64_viewport.julia_im_raw = vp.julia_im_raw;
    cp.fx64_viewport.bailout_raw = vp.bailout_raw;
    cp.fx64_viewport.bailout2_raw = vp.bailout2_raw;
    cp.fx64_viewport.two_raw = vp.two_raw;
    cp.fx64_viewport.two_sqrt2_floor_raw = vp.two_sqrt2_floor_raw;
    cp.fx64_viewport.bailout2_q57 = vp.bailout2_raw;
}

static double render_tile_gpu(
    const MapParams& base, const Tile& t,
    cv::Mat& out, bool use_fx,
    const std::string& fixed_scalar_type
) {
    MapParams p = base;
    const double aspect  = map_viewport_aspect(base);
    const double span_re = base.scale * aspect;
    const double span_im = base.scale;
    const double re_step = span_re / base.width;
    const double im_step = span_im / base.height;
    const double tile_dx = (static_cast<double>(t.x) + t.w * 0.5 -
                            base.width * 0.5) * re_step;
    const double tile_dy = -(static_cast<double>(t.y) + t.h * 0.5 -
                             base.height * 0.5) * im_step;
    const double rotation_rad = base.rotation_deg * M_PI / 180.0;
    const double cos_rotation = std::cos(rotation_rad);
    const double sin_rotation = std::sin(rotation_rad);

    fsd_cuda::CudaMapParams cp;
    cp.center_re  = base.center_re + tile_dx * cos_rotation - tile_dy * sin_rotation;
    cp.center_im  = base.center_im + tile_dx * sin_rotation + tile_dy * cos_rotation;
    cp.scale      = t.h * im_step;
    cp.viewport_aspect = (t.w * re_step) / (t.h * im_step);
    cp.width      = t.w;
    cp.height     = t.h;
    cp.iterations = base.iterations;
    cp.bailout    = base.bailout;
    cp.bailout_sq = base.bailout_sq;
    cp.scalar_type  = use_fx ? fixed_scalar_type : "fp64";
    cp.colormap_id  = static_cast<int>(base.colormap);
    cp.variant_id   = static_cast<int>(base.variant);
    cp.julia        = base.julia;
    cp.julia_re     = base.julia_re;
    cp.julia_im     = base.julia_im;
    cp.metric_id    = static_cast<int>(base.metric);
    cp.rotation_deg = base.rotation_deg;
    if (use_fx && fixed_scalar_type == "q3.60") {
        const FixedViewportRaw<60> vp = make_fixed_viewport_raw<60>(
            cp.center_re, cp.center_im, cp.scale, cp.viewport_aspect, cp.width, cp.height,
            cp.julia_re, cp.julia_im, cp.bailout, cp.bailout_sq);
        assign_cuda_fixed_viewport(cp, vp);
    } else if (use_fx && fixed_scalar_type == "q4.59") {
        const FixedViewportRaw<59> vp = make_fixed_viewport_raw<59>(
            cp.center_re, cp.center_im, cp.scale, cp.viewport_aspect, cp.width, cp.height,
            cp.julia_re, cp.julia_im, cp.bailout, cp.bailout_sq);
        assign_cuda_fixed_viewport(cp, vp);
    } else if (use_fx) {
        const FixedViewportRaw<57> vp = make_fixed_viewport_raw<57>(
            cp.center_re, cp.center_im, cp.scale, cp.viewport_aspect, cp.width, cp.height,
            cp.julia_re, cp.julia_im, cp.bailout, cp.bailout_sq);
        assign_cuda_fixed_viewport(cp, vp);
    }

    // cuda_render_map does cudaMemcpy into a contiguous buffer; passing a submat
    // (whose rows are separated by the full-image stride) would corrupt adjacent
    // tiles.  Render into a fresh contiguous Mat and then blit into the submat.
    cv::Mat tile_buf;
    auto stats = fsd_cuda::cuda_render_map(cp, tile_buf);
    tile_buf.copyTo(out(cv::Rect(t.x, t.y, t.w, t.h)));
    return stats.elapsed_ms;
}
#endif

// ---- Main scheduler ----

TileSchedulerStats render_map_hybrid(
    const MapParams& p, cv::Mat& out,
    int tile_size, TileCallback on_tile_done
) {
    if (out.empty() || out.rows != p.height || out.cols != p.width || out.type() != CV_8UC3) {
        out.create(p.height, p.width, CV_8UC3);
    }

    auto tiles = make_tiles(p.width, p.height, tile_size);
    const size_t n = tiles.size();

    std::atomic<size_t> next_tile{0};
    std::atomic<bool> cancelled{false};
    std::exception_ptr first_error;
    std::mutex error_mutex;
    auto cancel_requested = [&]() {
        if (cancelled.load(std::memory_order_relaxed)) return true;
        if (map_render_cancel_requested(p)) {
            cancelled.store(true, std::memory_order_relaxed);
            return true;
        }
        return false;
    };
    auto record_worker_exception = [&](std::exception_ptr ep) {
        cancelled.store(true, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(error_mutex);
        if (!first_error) first_error = ep;
    };

    const bool requested_fx = requested_fixed(p);
    const bool fx       = requested_fx && supports_fx64_int_path(p);
    const std::string fixed_scalar_type =
        requested_q360(p) ? "q3.60" : (requested_q459(p) ? "q4.59" : "fx64");
    const bool avx512_ok = map_engine_supported(p, "avx512", fx);
    const bool avx2_ok = map_engine_supported(p, "avx2", fx);
    const bool use_avx512 = avx512_ok;
    const bool use_avx2 = !use_avx512 && avx2_ok;
    const bool use_gpu  = false
#if USE_CUDA
                       || map_engine_supported(p, "cuda", fx)
#endif
                       ;
    const int n_cpu_threads = default_render_threads();
    const std::string cpu_engine = use_avx512 ? "avx512" : (use_avx2 ? "avx2" : "openmp");
    const std::string profile_scalar = fx ? "fx64" : "fp64";
    const size_t fallback_gpu_batch = runtime_capabilities().cuda_low_end ? 4u : 12u;
    const size_t gpu_batch_tiles = calibrated_gpu_batch(
        p, tile_size, profile_scalar, cpu_engine, n_cpu_threads, n, fallback_gpu_batch);

    TileSchedulerStats result;
    std::mutex stats_mutex;
    double total_cpu_ms = 0.0, total_gpu_ms = 0.0;
    int cpu_tiles = 0, gpu_tiles = 0;

    const auto t_start = std::chrono::steady_clock::now();

    // GPU worker thread (if available)
    std::thread gpu_thread;
    if (use_gpu) {
        gpu_thread = std::thread([&]() {
            try {
                while (true) {
                    if (cancel_requested()) break;
                    const size_t first = next_tile.fetch_add(gpu_batch_tiles);
                    if (first >= n) break;
                    const size_t last = std::min(n, first + gpu_batch_tiles);
                    for (size_t idx = first; idx < last; ++idx) {
                        if (cancel_requested()) break;
                        if (idx >= n) break;
                        const Tile& tile = tiles[idx];
#if USE_CUDA
                        double ms = 0.0;
                        bool rendered_on_gpu = true;
                        try {
                            ms = render_tile_gpu(p, tile, out, fx, fixed_scalar_type);
                        } catch (...) {
                            rendered_on_gpu = false;
                            ms = render_tile_cpu(p, tile, out, use_avx2, use_avx512, fx, fixed_scalar_type);
                        }
#else
                        (void)tile;
                        const double ms = 0.0;
                        const bool rendered_on_gpu = false;
#endif
                        {
                            std::lock_guard<std::mutex> lk(stats_mutex);
                            if (rendered_on_gpu) {
                                total_gpu_ms += ms;
                                gpu_tiles++;
                            } else {
                                total_cpu_ms += ms;
                                cpu_tiles++;
                            }
                        }
                        if (on_tile_done) on_tile_done(tile.x, tile.y, tile.w, tile.h);
                    }
                }
            } catch (const std::exception& ex) {
                cancelled.store(true, std::memory_order_relaxed);
                if (!is_cancelled_exception(ex)) record_worker_exception(std::current_exception());
            } catch (...) {
                record_worker_exception(std::current_exception());
            }
        });
    }

    // CPU workers — one thread per available CPU core
    std::vector<std::thread> cpu_threads;
    cpu_threads.reserve(static_cast<size_t>(n_cpu_threads));

    for (int tid = 0; tid < n_cpu_threads; tid++) {
        cpu_threads.emplace_back([&]() {
            try {
                while (true) {
                    if (cancel_requested()) break;
                    const size_t idx = next_tile.fetch_add(1);
                    if (idx >= n) break;
                    const Tile& tile = tiles[idx];
                    const double ms = render_tile_cpu(p, tile, out, use_avx2, use_avx512, fx, fixed_scalar_type);
                    {
                        std::lock_guard<std::mutex> lk(stats_mutex);
                        total_cpu_ms += ms;
                        cpu_tiles++;
                    }
                    if (on_tile_done) on_tile_done(tile.x, tile.y, tile.w, tile.h);
                }
            } catch (const std::exception& ex) {
                cancelled.store(true, std::memory_order_relaxed);
                if (!is_cancelled_exception(ex)) record_worker_exception(std::current_exception());
            } catch (...) {
                record_worker_exception(std::current_exception());
            }
        });
    }

    // Wait for all workers
    for (auto& t : cpu_threads) t.join();
    if (gpu_thread.joinable()) gpu_thread.join();
    if (first_error) std::rethrow_exception(first_error);
    if (cancelled.load(std::memory_order_relaxed) || map_render_cancel_requested(p)) {
        throw std::runtime_error("cancelled");
    }

    const auto t_end = std::chrono::steady_clock::now();

    result.total_ms   = std::chrono::duration<double,std::milli>(t_end - t_start).count();
    result.cpu_ms     = total_cpu_ms;
    result.gpu_ms     = total_gpu_ms;
    result.cpu_tiles  = cpu_tiles;
    result.gpu_tiles  = gpu_tiles;
    result.scalar_used = fx ? fixed_scalar_type : "fp64";

    if (use_gpu && gpu_tiles > 0 && cpu_tiles > 0)
        result.engine_used = "hybrid";
    else if (use_gpu && gpu_tiles > 0)
        result.engine_used = "cuda";
    else if (use_avx512)
        result.engine_used = "avx512";
    else if (use_avx2)
        result.engine_used = "avx2";
    else
        result.engine_used = "openmp";

    return result;
}

// ---- Field-output hybrid scheduler ----

static double render_tile_cpu_field(
    const MapParams& base, const Tile& t,
    FieldOutput& fo, bool use_avx2, bool use_avx512
) {
    MapParams p = base;
    const double aspect  = map_viewport_aspect(base);
    const double span_im = base.scale;
    const double span_re = base.scale * aspect;
    const double re_step = span_re / base.width;
    const double im_step = span_im / base.height;
    const double tile_dx = (static_cast<double>(t.x) + t.w * 0.5 -
                            base.width * 0.5) * re_step;
    const double tile_dy = -(static_cast<double>(t.y) + t.h * 0.5 -
                             base.height * 0.5) * im_step;
    const double rotation_rad = base.rotation_deg * M_PI / 180.0;
    const double cos_rotation = std::cos(rotation_rad);
    const double sin_rotation = std::sin(rotation_rad);

    p.center_re = base.center_re + tile_dx * cos_rotation - tile_dy * sin_rotation;
    p.center_im = base.center_im + tile_dx * sin_rotation + tile_dy * cos_rotation;
    p.scale     = t.h * im_step;
    p.viewport_aspect = (t.w * re_step) / (t.h * im_step);
    p.width     = t.w;
    p.height    = t.h;
    p.engine    = use_avx512 ? "avx512" : (use_avx2 ? "avx2" : "openmp");
    p.scalar_type = "fp64";
    p.render_threads = 1;

    FieldOutput tile_fo;
    MapStats stats;
    if (use_avx512) {
        stats = render_map_field_avx512_fp64(p, tile_fo);
    } else if (use_avx2) {
        stats = render_map_avx2_field(p, tile_fo);
    } else {
        stats = render_map_field(p, tile_fo);
    }

    const int W = base.width;
    for (int row = 0; row < t.h; ++row) {
        const size_t src_off = static_cast<size_t>(row) * static_cast<size_t>(t.w);
        const size_t dst_off = static_cast<size_t>(t.y + row) * static_cast<size_t>(W) + static_cast<size_t>(t.x);
        std::copy_n(tile_fo.iter_u32.data() + src_off, static_cast<size_t>(t.w), fo.iter_u32.data() + dst_off);
        std::copy_n(tile_fo.norm_f32.data() + src_off, static_cast<size_t>(t.w), fo.norm_f32.data() + dst_off);
    }
    return stats.elapsed_ms;
}

#if USE_CUDA
static double render_tile_gpu_field(
    const MapParams& base, const Tile& t,
    FieldOutput& fo
) {
    const double aspect  = map_viewport_aspect(base);
    const double span_re = base.scale * aspect;
    const double span_im = base.scale;
    const double re_step = span_re / base.width;
    const double im_step = span_im / base.height;
    const double tile_dx = (static_cast<double>(t.x) + t.w * 0.5 -
                            base.width * 0.5) * re_step;
    const double tile_dy = -(static_cast<double>(t.y) + t.h * 0.5 -
                             base.height * 0.5) * im_step;
    const double rotation_rad = base.rotation_deg * M_PI / 180.0;
    const double cos_rotation = std::cos(rotation_rad);
    const double sin_rotation = std::sin(rotation_rad);

    fsd_cuda::CudaMapParams cp;
    cp.center_re  = base.center_re + tile_dx * cos_rotation - tile_dy * sin_rotation;
    cp.center_im  = base.center_im + tile_dx * sin_rotation + tile_dy * cos_rotation;
    cp.scale      = t.h * im_step;
    cp.viewport_aspect = (t.w * re_step) / (t.h * im_step);
    cp.width      = t.w;
    cp.height     = t.h;
    cp.iterations = base.iterations;
    cp.bailout    = base.bailout;
    cp.bailout_sq = base.bailout_sq;
    cp.scalar_type  = "fp64";
    cp.colormap_id  = static_cast<int>(base.colormap);
    cp.variant_id   = static_cast<int>(base.variant);
    cp.julia        = base.julia;
    cp.julia_re     = base.julia_re;
    cp.julia_im     = base.julia_im;
    cp.metric_id    = 0;
    cp.rotation_deg = base.rotation_deg;

    const size_t tile_n = static_cast<size_t>(t.w) * static_cast<size_t>(t.h);
    std::vector<uint32_t> tile_iter(tile_n, 0u);
    std::vector<float>    tile_norm(tile_n, 0.0f);
    auto cs = fsd_cuda::cuda_render_map_field(cp, tile_iter.data(), tile_norm.data());

    const int W = base.width;
    for (int row = 0; row < t.h; ++row) {
        const size_t src_off = static_cast<size_t>(row) * static_cast<size_t>(t.w);
        const size_t dst_off = static_cast<size_t>(t.y + row) * static_cast<size_t>(W) + static_cast<size_t>(t.x);
        std::copy_n(tile_iter.data() + src_off, static_cast<size_t>(t.w), fo.iter_u32.data() + dst_off);
        std::copy_n(tile_norm.data() + src_off, static_cast<size_t>(t.w), fo.norm_f32.data() + dst_off);
    }
    return cs.elapsed_ms;
}
#endif

TileSchedulerStats render_map_field_hybrid(
    const MapParams& p, FieldOutput& fo, int tile_size
) {
    const size_t n_pixels = static_cast<size_t>(p.width) * static_cast<size_t>(p.height);
    fo.width  = p.width;
    fo.height = p.height;
    fo.metric = Metric::Escape;
    fo.iter_u32.assign(n_pixels, 0u);
    fo.norm_f32.assign(n_pixels, 0.0f);

    auto tiles = make_tiles(p.width, p.height, tile_size);
    const size_t n = tiles.size();

    std::atomic<size_t> next_tile{0};
    std::atomic<bool> cancelled{false};
    std::exception_ptr first_error;
    std::mutex error_mutex;
    auto cancel_requested = [&]() {
        if (cancelled.load(std::memory_order_relaxed)) return true;
        if (map_render_cancel_requested(p)) {
            cancelled.store(true, std::memory_order_relaxed);
            return true;
        }
        return false;
    };
    auto record_worker_exception = [&](std::exception_ptr ep) {
        cancelled.store(true, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(error_mutex);
        if (!first_error) first_error = ep;
    };

    const bool avx512_ok = map_engine_supported(p, "avx512", false);
    const bool avx2_ok   = map_engine_supported(p, "avx2", false);
    const bool use_avx512 = avx512_ok;
    const bool use_avx2   = !use_avx512 && avx2_ok;
    const bool use_gpu    = false
#if USE_CUDA
                         || map_engine_supported(p, "cuda", false)
#endif
                         ;
    const int n_cpu_threads = default_render_threads();
    const std::string cpu_engine = use_avx512 ? "avx512" : (use_avx2 ? "avx2" : "openmp");
    const size_t fallback_gpu_batch = runtime_capabilities().cuda_low_end ? 4u : 12u;
    const size_t gpu_batch = calibrated_gpu_batch(
        p, tile_size, "fp64", cpu_engine, n_cpu_threads, n, fallback_gpu_batch);

    TileSchedulerStats result;
    std::mutex stats_mutex;
    double total_cpu_ms = 0.0, total_gpu_ms = 0.0;
    int cpu_tiles = 0, gpu_tiles = 0;

    const auto t_start = std::chrono::steady_clock::now();

    std::thread gpu_thread;
    if (use_gpu) {
        gpu_thread = std::thread([&]() {
            try {
                while (true) {
                    if (cancel_requested()) break;
                    const size_t first = next_tile.fetch_add(gpu_batch);
                    if (first >= n) break;
                    const size_t last = std::min(n, first + gpu_batch);
                    for (size_t idx = first; idx < last; ++idx) {
                        if (cancel_requested()) break;
                        if (idx >= n) break;
                        const Tile& tile = tiles[idx];
                        double ms = 0.0;
#if USE_CUDA
                        bool rendered_on_gpu = true;
                        try {
                            ms = render_tile_gpu_field(p, tile, fo);
                        } catch (...) {
                            rendered_on_gpu = false;
                            ms = render_tile_cpu_field(p, tile, fo, use_avx2, use_avx512);
                        }
#else
                        (void)tile;
                        const bool rendered_on_gpu = false;
#endif
                        {
                            std::lock_guard<std::mutex> lk(stats_mutex);
                            if (rendered_on_gpu) {
                                total_gpu_ms += ms;
                                gpu_tiles++;
                            } else {
                                total_cpu_ms += ms;
                                cpu_tiles++;
                            }
                        }
                    }
                }
            } catch (const std::exception& ex) {
                cancelled.store(true, std::memory_order_relaxed);
                if (!is_cancelled_exception(ex)) record_worker_exception(std::current_exception());
            } catch (...) {
                record_worker_exception(std::current_exception());
            }
        });
    }

    std::vector<std::thread> cpu_threads;
    cpu_threads.reserve(static_cast<size_t>(n_cpu_threads));

    for (int tid = 0; tid < n_cpu_threads; tid++) {
        cpu_threads.emplace_back([&]() {
            try {
                while (true) {
                    if (cancel_requested()) break;
                    const size_t idx = next_tile.fetch_add(1);
                    if (idx >= n) break;
                    const Tile& tile = tiles[idx];
                    const double ms = render_tile_cpu_field(p, tile, fo, use_avx2, use_avx512);
                    {
                        std::lock_guard<std::mutex> lk(stats_mutex);
                        total_cpu_ms += ms;
                        cpu_tiles++;
                    }
                }
            } catch (const std::exception& ex) {
                cancelled.store(true, std::memory_order_relaxed);
                if (!is_cancelled_exception(ex)) record_worker_exception(std::current_exception());
            } catch (...) {
                record_worker_exception(std::current_exception());
            }
        });
    }

    for (auto& t : cpu_threads) t.join();
    if (gpu_thread.joinable()) gpu_thread.join();
    if (first_error) std::rethrow_exception(first_error);
    if (cancelled.load(std::memory_order_relaxed) || map_render_cancel_requested(p)) {
        throw std::runtime_error("cancelled");
    }

    const auto t_end = std::chrono::steady_clock::now();

    result.total_ms   = std::chrono::duration<double,std::milli>(t_end - t_start).count();
    result.cpu_ms     = total_cpu_ms;
    result.gpu_ms     = total_gpu_ms;
    result.cpu_tiles  = cpu_tiles;
    result.gpu_tiles  = gpu_tiles;
    result.scalar_used = "fp64";

    if (use_gpu && gpu_tiles > 0 && cpu_tiles > 0)
        result.engine_used = "hybrid";
    else if (use_gpu && gpu_tiles > 0)
        result.engine_used = "cuda";
    else if (use_avx512)
        result.engine_used = "avx512";
    else if (use_avx2)
        result.engine_used = "avx2";
    else
        result.engine_used = "openmp";

    fo.scalar_used = result.scalar_used;
    fo.engine_used = result.engine_used;
    return result;
}

} // namespace fsd::compute
