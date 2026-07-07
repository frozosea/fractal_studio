// compute/cuda/ln_map.cuh

#pragma once

#include <opencv2/core.hpp>
#include <cstdint>

namespace fsd_cuda {

struct CudaLnMapParams {
    bool julia = false;
    double center_re = 0.0;
    double center_im = 0.0;
    double julia_re = 0.0;
    double julia_im = 0.0;
    int width_s = 1024;
    int height_t = 4096;
    double row_offset = 0.0;
    int iterations = 2048;
    double bailout = 2.0;
    double bailout_sq = 4.0;
    int variant_id = 0;
    int colormap_id = 0;
};

struct CudaLnMapStats {
    double elapsed_ms = 0.0;
};

bool cuda_ln_map_available() noexcept;
CudaLnMapStats cuda_render_ln_map(const CudaLnMapParams& p, cv::Mat& out);
CudaLnMapStats cuda_render_ln_map_rows(const CudaLnMapParams& p, cv::Mat& out, int row_start, int row_count);
CudaLnMapStats cuda_render_ln_map_fp32(const CudaLnMapParams& p, cv::Mat& out);
CudaLnMapStats cuda_render_ln_map_fp32_rows(const CudaLnMapParams& p, cv::Mat& out, int row_start, int row_count);
CudaLnMapStats cuda_render_ln_map_fx64(const CudaLnMapParams& p, cv::Mat& out);
CudaLnMapStats cuda_render_ln_map_fx64_rows(const CudaLnMapParams& p, cv::Mat& out, int row_start, int row_count);
// Raw escape-count field (int per pixel) on the GPU, fp64 (fx64=false) or fx64 (fx64=true).
CudaLnMapStats cuda_render_ln_map_iters_rows(const CudaLnMapParams& p, int* iters, int row_start, int row_count, bool fx64);
// Raw escape-count field at fp32 precision (for the fast mode's shallow rows).
CudaLnMapStats cuda_render_ln_map_iters_fp32_rows(const CudaLnMapParams& p, int* iters, int row_start, int row_count);
// GPU-side escape-count histogram accumulation for ln-map equalization. These
// compute a temporary device escape-count field and return only compact
// histograms/row flags to the host.
CudaLnMapStats cuda_accumulate_ln_map_histograms(
    const CudaLnMapParams& p,
    uint32_t* disk_hist,
    uint32_t* all_hist,
    uint32_t* disk_rows,
    uint32_t* all_rows,
    int row_start,
    int row_count,
    bool fx64);
CudaLnMapStats cuda_accumulate_ln_map_histograms_fp32(
    const CudaLnMapParams& p,
    uint32_t* disk_hist,
    uint32_t* all_hist,
    uint32_t* disk_rows,
    uint32_t* all_rows,
    int row_start,
    int row_count);

} // namespace fsd_cuda
