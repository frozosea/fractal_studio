// compute/cuda/map_kernel.cuh
//
// Host-side interface to the CUDA map renderer.
// Includes fp32, fp64, and fx64 variants for all 10 fractal variants,
// Julia mode, and non-escape metrics (MinAbs, MaxAbs, Envelope).

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

// Forward-declare OpenCV types without pulling in all headers.
namespace cv { class Mat; }

namespace fsd_cuda {

struct Fx64ViewportRaw {
    int64_t first_re_raw = 0;
    int64_t first_im_raw = 0;
    int64_t step_re_raw = 0;
    int64_t step_im_raw = 0;
    int64_t julia_re_raw = 0;
    int64_t julia_im_raw = 0;
    uint64_t bailout_raw = 0;
    uint64_t bailout2_raw = 0;
    uint64_t two_raw = 0;
    uint64_t two_sqrt2_floor_raw = 0;
    uint64_t bailout2_q57 = 0;
};

struct CudaMapParams {
    double center_re = -0.75;
    double center_im =  0.0;
    double scale     =  3.0;

    int width       = 1024;
    int height      = 768;
    int iterations  = 1024;
    double bailout  = 2.0;  // radius, kept for metric normalization
    double bailout_sq = 4.0; // squared threshold used by escape tests

    // "fp32", "fp64", "fx64"/"q6.57", "q4.59", or experimental "q3.60"
    std::string scalar_type = "fp64";

    // Colormap ID — must match fsd::compute::Colormap enum values:
    //   0=ClassicCos, 1=Mod17, 2=HsvWheel, 3=Tri765, 4=Grayscale
    //   (LnSmooth=5 is never passed here; handled by CPU path)
    int colormap_id = 0;

    // Variant ID — matches fsd::compute::Variant enum:
    //   0=Mandelbrot, 1=Tri, 2=Boat, 3=Duck, 4=Bell,
    //   5=Fish, 6=Vase, 7=Bird, 8=Mask, 9=Ship
    int variant_id = 0;

    double rotation_deg = 0.0;

    // Julia mode: if true, z0 = pixel, c = (julia_re, julia_im)
    bool julia    = false;
    double julia_re = 0.0;
    double julia_im = 0.0;

    // Metric ID — matches fsd::compute::Metric enum:
    //   0=Escape, 1=MinAbs, 2=MaxAbs, 3=Envelope
    //   (MinPairwiseDist=4 is NOT supported on CUDA — use CPU path)
    int metric_id = 0;

    // Host-precomputed fixed-point viewport for the integer kernel.
    Fx64ViewportRaw fx64_viewport;
};

struct CudaMapStats {
    double elapsed_ms = 0.0;
    std::string scalar_used;
    std::string engine_used = "cuda";
};

struct CudaDeviceInfo {
    bool available = false;
    int device_count = 0;
    int major = 0;
    int minor = 0;
    size_t total_global_mem = 0;
    size_t free_global_mem = 0;
    std::string name;
};

// Returns true if a CUDA device is present and initialised.
bool cuda_available() noexcept;
CudaDeviceInfo cuda_device_info() noexcept;

// Render a fractal map using the CUDA kernels.
// Output `out` is allocated as CV_8UC3 BGR on the host.
CudaMapStats cuda_render_map(const CudaMapParams& p, cv::Mat& out);

// Escape-count FIELD: writes W*H raw iteration counts (uint32) and |z|² at escape (float)
// into caller-provided host buffers, instead of BGR. Escape metric, fp32/fp64 only (feeds
// equalized coloring). Mirrors cuda_render_map's scalar/variant dispatch.
CudaMapStats cuda_render_map_field(const CudaMapParams& p, uint32_t* iter_u32, float* norm_f32);

} // namespace fsd_cuda
