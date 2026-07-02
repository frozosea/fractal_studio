// compute/cuda/video_warp.cuh

#pragma once

#include <opencv2/core.hpp>

#include <cstddef>
#include <cstdint>

namespace fsd_cuda {

struct CudaVideoWarpContext {
    int width = 0;
    int height = 0;
    int strip_width = 0; // includes one wrap column
    int strip_height = 0;
    void* d_strip = nullptr;
    void* d_final = nullptr;
    void* d_geom = nullptr;
    void* d_out = nullptr;
    void* d_out_alt = nullptr;
    void* strip_array = nullptr;
    void* final_array = nullptr;
    uint64_t strip_tex = 0;
    uint64_t final_tex = 0;
    void* kernel_start_event = nullptr;
    void* kernel_stop_event = nullptr;
    void* streams[2] = {nullptr, nullptr};
    void* async_kernel_start_events[2] = {nullptr, nullptr};
    void* async_kernel_stop_events[2] = {nullptr, nullptr};
    void* async_copy_start_events[2] = {nullptr, nullptr};
    void* async_copy_stop_events[2] = {nullptr, nullptr};
};

struct CudaVideoWarpTiming {
    double kernel_ms = 0.0;
    double copy_ms = 0.0;
};

bool cuda_video_warp_available() noexcept;
void cuda_video_warp_init(const cv::Mat& stripWrap, const cv::Mat& finalImg, double rotationDeg, CudaVideoWarpContext& ctx);
void cuda_video_warp_frame_timed(CudaVideoWarpContext& ctx, double kTop, double kTopEnd, cv::Mat& frame, CudaVideoWarpTiming* timing);
void cuda_video_warp_frame(CudaVideoWarpContext& ctx, double kTop, double kTopEnd, cv::Mat& frame);
void* cuda_video_warp_alloc_pinned(size_t bytes);
void cuda_video_warp_free_pinned(void* ptr) noexcept;
void cuda_video_warp_frame_async(CudaVideoWarpContext& ctx, double kTop, double kTopEnd, int bufferIndex, void* hostPtr);
void cuda_video_warp_wait_frame(CudaVideoWarpContext& ctx, int bufferIndex, CudaVideoWarpTiming* timing);
void cuda_video_warp_release(CudaVideoWarpContext& ctx) noexcept;

} // namespace fsd_cuda
