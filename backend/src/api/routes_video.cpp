// routes_video.cpp
//
// Zoom video export from a ln-map artifact + a final rendered cartesian frame.
//
// Design
// ------
// The ln-map strip samples (angle θ, log-radius k) space around `center`.
// A video frame at zoom level `kTop` maps screen pixel (u, v) to:
//
//   world point c = center + (u + iv)·e^kTop          (u,v normalised)
//   strip col     = (θ / 2π) · s                      (wraps)
//   strip row     = (ln4 − kTop − ln(r_screen)) · s/τ
//
// where r_screen = |u+iv|. The corner pixel (r_screen = r_max = √(aspect²+1))
// hits strip row 0 when kTop_start = ln4 − ln(r_max). Starting there ensures
// no negative-row out-of-bounds at the first frame.
//
// The screen centre (r_screen→0) would require row→∞ — beyond the strip.
// That region is filled from a cartesian final image rendered directly at
// kTop_end = kTop_start − depth·ln2.
//
// Compositing rule (per pixel, per frame):
//   strip_row ∈ [0, stripH)  →  use bilinear sample from strip
//   else                      →  use bilinear sample from final image
//                                scaled by e^(kTop − kTop_end)

#include "routes.hpp"
#include "routes_common.hpp"
#include "resource_manager.hpp"

#include "../compute/image_io.hpp"
#include "../compute/ln_map.hpp"
#include "../compute/map_kernel.hpp"
#include "../compute/variants.hpp"
#include "../compute/colormap.hpp"

#if defined(HAS_CUDA_KERNEL)
#  include "../compute/cuda/video_warp.cuh"
#  define USE_CUDA_VIDEO_WARP 1
#else
#  define USE_CUDA_VIDEO_WARP 0
#endif

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fsd {

namespace {

constexpr double TAU     = 6.283185307179586;
constexpr double PI      = 3.141592653589793;
constexpr double LN_TWO  = 0.6931471805599453;
constexpr double LN_FOUR = 1.3862943611198906;
constexpr int    MIN_VIDEO_DIM = 128;
constexpr int    MAX_VIDEO_DIM = 8192;
constexpr int64_t MAX_VIDEO_PIXELS = 7680LL * 4320LL;
constexpr double DEFAULT_SECONDS_PER_OCTAVE = 0.4;
constexpr double MAX_SECONDS_PER_OCTAVE = 60.0;
constexpr double MAX_VIDEO_DURATION_SEC = 3600.0;
constexpr int OPENCV_REMAP_DIM_LIMIT = std::numeric_limits<short>::max();

int roundUpToMultiple(int value, int multiple) {
    if (multiple <= 1) return value;
    const int rem = value % multiple;
    return rem == 0 ? value : value + (multiple - rem);
}

int derivedMinStripWidth(int W, int H) {
    const double diag = std::sqrt(static_cast<double>(W) * static_cast<double>(W)
                                + static_cast<double>(H) * static_cast<double>(H));
    const int minWidth = static_cast<int>(std::ceil(diag * PI));
    return roundUpToMultiple(minWidth, 8);
}

struct StripPlan {
    int fullWidthS = 0;
    int actualWidthS = 0;
    int heightT = 0;
    double extraOctaves = 2.0;
    double qualityScale = 1.0;
    std::string qualityPreset = "full";
    uint64_t estimatedPeakMemory = 0;
};

double presetQualityScale(const std::string& preset) {
    if (preset == "draft") return 0.35;
    if (preset == "balanced") return 0.55;
    if (preset == "high") return 0.75;
    if (preset == "full") return 1.0;
    return 0.55;
}

std::string defaultQualityPresetForSize(int W, int H) {
    return (W >= 3840 || H >= 2160) ? "balanced" : "high";
}

uint64_t estimateVideoPeakMemoryBytes(int W, int H, int s, int t) {
    const uint64_t pixels = static_cast<uint64_t>(W) * static_cast<uint64_t>(H);
    const uint64_t stripPixels = static_cast<uint64_t>(s) * static_cast<uint64_t>(t);
    const uint64_t frameBgr = pixels * 3u;
    const uint64_t remapF32 = pixels * 4u;
    const uint64_t stripBgr = stripPixels * 3u;
    return stripBgr + frameBgr * 4u + remapF32 * 5u + pixels;
}

bool commandSucceeds(const std::string& command) {
    return std::system(command.c_str()) == 0;
}

struct VideoWarpStats {
    double warpTotalMs = 0.0;
    double copyTotalMs = 0.0;
    double writeTotalMs = 0.0;
    double encodeCloseMs = 0.0;
    uint64_t rawVideoBytes = 0;
    int frameCount = 0;
    int stripWidth = 0;
    int stripHeight = 0;
    bool opencvRemapSafe = false;
    std::string warpMethod;
    std::string encoder;
};

Json videoWarpStatsJson(const VideoWarpStats& stats) {
    const double frames = static_cast<double>(std::max(1, stats.frameCount));
    return Json{
        {"warpTotalMs", stats.warpTotalMs},
        {"copyTotalMs", stats.copyTotalMs},
        {"writeTotalMs", stats.writeTotalMs},
        {"encodeCloseMs", stats.encodeCloseMs},
        {"avgWarpMs", stats.warpTotalMs / frames},
        {"avgCopyMs", stats.copyTotalMs / frames},
        {"avgWriteMs", stats.writeTotalMs / frames},
        {"rawVideoBytes", stats.rawVideoBytes},
        {"frameCount", stats.frameCount},
        {"stripWidth", stats.stripWidth},
        {"stripHeight", stats.stripHeight},
        {"opencvRemapSafe", stats.opencvRemapSafe},
        {"warpMethod", stats.warpMethod},
        {"encoder", stats.encoder},
    };
}

void mergeVideoWarpStats(Json& dst, const VideoWarpStats& stats) {
    const Json src = videoWarpStatsJson(stats);
    for (auto it = src.begin(); it != src.end(); ++it) {
        dst[it.key()] = it.value();
    }
}

bool opencv_remap_size_safe(const cv::Mat& src, int dstW, int dstH) {
    return !src.empty()
        && src.cols > 0 && src.rows > 0 && dstW > 0 && dstH > 0
        && src.cols < OPENCV_REMAP_DIM_LIMIT
        && src.rows < OPENCV_REMAP_DIM_LIMIT
        && dstW < OPENCV_REMAP_DIM_LIMIT
        && dstH < OPENCV_REMAP_DIM_LIMIT;
}

StripPlan resolveStripPlan(const Json& j, int W, int H, double depthOctaves, const std::string& colorMode) {
    StripPlan plan;
    plan.fullWidthS = derivedMinStripWidth(W, H);
    plan.qualityPreset = j.value("qualityPreset", defaultQualityPresetForSize(W, H));
    plan.qualityScale = j.value("qualityScale", presetQualityScale(plan.qualityPreset));
    if (!(plan.qualityScale > 0.0) || plan.qualityScale > 1.0 || !std::isfinite(plan.qualityScale)) {
        throw std::runtime_error("invalid qualityScale (0..1)");
    }

    int requested = 0;
    if (j.contains("widthS") && !j["widthS"].is_null()) {
        requested = j.value("widthS", plan.fullWidthS);
        plan.qualityPreset = "custom";
        plan.qualityScale = static_cast<double>(requested) / std::max(1, plan.fullWidthS);
    } else {
        requested = static_cast<int>(std::ceil(static_cast<double>(plan.fullWidthS) * plan.qualityScale));
    }
    plan.actualWidthS = roundUpToMultiple(std::max(128, requested), 8);
    const double defaultExtra = colorMode == "escape" ? 2.0 : 7.0;
    plan.extraOctaves = j.value("lnMapExtraOctaves", defaultExtra);
    if (!(plan.extraOctaves >= 2.0) || plan.extraOctaves > 16.0 || !std::isfinite(plan.extraOctaves)) {
        throw std::runtime_error("invalid lnMapExtraOctaves (2..16)");
    }
    const double t_exact = (plan.extraOctaves + depthOctaves) * LN_TWO / TAU * static_cast<double>(plan.actualWidthS);
    plan.heightT = static_cast<int>(std::ceil(t_exact));
    plan.estimatedPeakMemory = estimateVideoPeakMemoryBytes(W, H, plan.actualWidthS, plan.heightT);
    return plan;
}

void validateVideoOutputSize(int W, int H) {
    if (W < MIN_VIDEO_DIM || H < MIN_VIDEO_DIM || W > MAX_VIDEO_DIM || H > MAX_VIDEO_DIM) {
        throw std::runtime_error("invalid output size (128..8192)");
    }
    const int64_t pixels = static_cast<int64_t>(W) * static_cast<int64_t>(H);
    if (pixels > MAX_VIDEO_PIXELS) {
        throw std::runtime_error("invalid output size (too many pixels; max 7680x4320 area)");
    }
}

std::pair<int, int> resolvePreviewSize(const Json& j, int W, int H) {
    int previewW = j.value("previewWidth", 0);
    int previewH = j.value("previewHeight", 0);
    if (previewW <= 0 || previewH <= 0) {
        constexpr double maxPreviewSide = 720.0;
        const double scale = std::min(1.0, maxPreviewSide / static_cast<double>(std::max(W, H)));
        previewW = std::max(64, static_cast<int>(std::llround(static_cast<double>(W) * scale)));
        previewH = std::max(64, static_cast<int>(std::llround(static_cast<double>(H) * scale)));
    }
    if (previewW < 64 || previewH < 64 || previewW > 2048 || previewH > 2048) {
        throw std::runtime_error("invalid preview size (64..2048)");
    }
    if (static_cast<int64_t>(previewW) * static_cast<int64_t>(previewH) > 1920LL * 1080LL) {
        throw std::runtime_error("invalid preview size (too many pixels)");
    }
    return { previewW, previewH };
}

double kTopStartForFrame(int W, int H) {
    const double aspect = static_cast<double>(W) / static_cast<double>(H);
    const double rMax   = std::sqrt(aspect * aspect + 1.0);
    return LN_FOUR - std::log(rMax);
}

double resolveDepthOctaves(const Json& j, double kTopStart, double fallbackDepth) {
    double depth = j.value("depthOctaves", fallbackDepth);
    if (j.contains("targetScale") && !j["targetScale"].is_null()) {
        const double targetScale = j.value("targetScale", 0.0);
        if (!(targetScale > 0.0) || !std::isfinite(targetScale)) {
            throw std::runtime_error("invalid targetScale");
        }
        const double targetKTop = std::log(targetScale * 0.5);
        depth = (kTopStart - targetKTop) / LN_TWO;
    }
    if (!(depth > 0.0) || !std::isfinite(depth)) {
        throw std::runtime_error("invalid depthOctaves");
    }
    return depth;
}

double resolveSecondsPerOctave(const Json& j, double depthOctaves) {
    double secondsPerOctave = DEFAULT_SECONDS_PER_OCTAVE;
    if (j.contains("secondsPerOctave") && !j["secondsPerOctave"].is_null()) {
        secondsPerOctave = j.value("secondsPerOctave", DEFAULT_SECONDS_PER_OCTAVE);
    } else if (j.contains("durationSec") && !j["durationSec"].is_null()) {
        const double durationSec = j.value("durationSec", 0.0);
        if (!(durationSec > 0.0) || !std::isfinite(durationSec)) {
            throw std::runtime_error("invalid durationSec");
        }
        secondsPerOctave = durationSec / std::max(depthOctaves, 1e-9);
    }
    if (!(secondsPerOctave > 0.0) || secondsPerOctave > MAX_SECONDS_PER_OCTAVE || !std::isfinite(secondsPerOctave)) {
        throw std::runtime_error("invalid secondsPerOctave (0..60)");
    }
    return secondsPerOctave;
}

int frameCountFromSpeed(double depthOctaves, double secondsPerOctave, int fps, double& durationSec) {
    durationSec = depthOctaves * secondsPerOctave;
    if (!(durationSec > 0.0) || durationSec > MAX_VIDEO_DURATION_SEC || !std::isfinite(durationSec)) {
        throw std::runtime_error("invalid video duration");
    }
    const long long frames = std::llround(durationSec * static_cast<double>(fps));
    if (frames < 2) return 2;
    if (frames > 10000000LL) throw std::runtime_error("too many video frames");
    return static_cast<int>(frames);
}

double bailoutSqFromJson(const Json& j, double radius, double defaultSq) {
    if (j.contains("bailoutSq") && !j["bailoutSq"].is_null()) {
        return j.value("bailoutSq", defaultSq);
    }
    if (j.contains("bailout") && !j["bailout"].is_null()) {
        return radius * radius;
    }
    return defaultSq;
}

struct CpuWarpGeometry {
    int width = 0;
    int height = 0;
    int strip_width = 0; // includes one wrap column
    int strip_height = 0;
    int strip_period = 0;
    cv::Mat strip_x;        // CV_32FC1, theta mapped to strip column
    cv::Mat strip_row_base; // CV_32FC1, (ln4 - ln(r)) * strip_period / tau
};

CpuWarpGeometry buildCpuWarpGeometry(int W, int H, int stripWrapW, int stripH) {
    CpuWarpGeometry geom;
    geom.width = W;
    geom.height = H;
    geom.strip_width = stripWrapW;
    geom.strip_height = stripH;
    geom.strip_period = stripWrapW - 1;
    geom.strip_x.create(H, W, CV_32FC1);
    geom.strip_row_base.create(H, W, CV_32FC1);

    const double aspect = static_cast<double>(W) / static_cast<double>(H);
    const double stripScale = static_cast<double>(geom.strip_period) / TAU;
    const float invalidRow = std::numeric_limits<float>::lowest();

    cv::parallel_for_(cv::Range(0, H), [&](const cv::Range& range) {
        for (int y = range.start; y < range.end; ++y) {
            const double vy = -(2.0 * (static_cast<double>(y) + 0.5) / static_cast<double>(H) - 1.0);
            float* stripX = geom.strip_x.ptr<float>(y);
            float* rowBase = geom.strip_row_base.ptr<float>(y);
            for (int x = 0; x < W; ++x) {
                const double ux = (2.0 * (static_cast<double>(x) + 0.5) / static_cast<double>(W) - 1.0) * aspect;
                const double r2 = ux * ux + vy * vy;
                double theta = std::atan2(vy, ux);
                if (theta < 0.0) theta += TAU;

                stripX[x] = static_cast<float>(theta * stripScale);
                rowBase[x] = r2 > 1e-30
                    ? static_cast<float>((LN_FOUR - 0.5 * std::log(r2)) * stripScale)
                    : invalidRow;
            }
        }
    });

    return geom;
}

void ensureMat(cv::Mat& mat, int W, int H, int type) {
    if (mat.empty() || mat.rows != H || mat.cols != W || mat.type() != type) {
        mat.create(H, W, type);
    }
}

double smoothstep01(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

uint8_t blendByte(uint8_t a, uint8_t b, double t) {
    const double v = static_cast<double>(a) * (1.0 - t) + static_cast<double>(b) * t;
    if (v <= 0.0) return 0;
    if (v >= 255.0) return 255;
    return static_cast<uint8_t>(v + 0.5);
}

void renderWarpFrameShared(
    const cv::Mat& stripWrap,
    const cv::Mat& finalImg,
    const CpuWarpGeometry& geom,
    int W,
    int H,
    double kTop, double kTop_end,
    cv::Mat& frame,
    cv::Mat& stripFrame,
    cv::Mat& finalFrame,
    cv::Mat& mapY,
    cv::Mat& fmapX,
    cv::Mat& fmapY
) {
    const int stripH    = stripWrap.rows;
    const int s         = stripWrap.cols - 1;
    const float stripLimit = static_cast<float>(stripH - 1);
    const float finalBlendRows = std::max(4.0f, static_cast<float>(s) / 128.0f);
    const float finalBlendStart = stripLimit - finalBlendRows;
    const float kTopStripScale = static_cast<float>(kTop * static_cast<double>(s) / TAU);
    const double S      = std::exp(kTop - kTop_end);

    for (int y = 0; y < H; ++y) {
        const float* rowBase = geom.strip_row_base.ptr<float>(y);
        float* rowMap = mapY.ptr<float>(y);
        float* fx = fmapX.ptr<float>(y);
        float* fy = fmapY.ptr<float>(y);
        const float finalY = static_cast<float>((static_cast<double>(y) + 0.5 - 0.5 * static_cast<double>(H)) * S
                                                + 0.5 * static_cast<double>(H));
        for (int x = 0; x < W; ++x) {
            const float stripY = rowBase[x] - kTopStripScale;
            rowMap[x] = (stripY >= 0.0f && stripY < stripLimit) ? stripY : -1.0f;
            fx[x] = static_cast<float>((static_cast<double>(x) + 0.5 - 0.5 * static_cast<double>(W)) * S
                                        + 0.5 * static_cast<double>(W));
            fy[x] = finalY;
        }
    }

    cv::remap(stripWrap, stripFrame, geom.strip_x, mapY,  cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0,0,0));
    cv::remap(finalImg,  finalFrame, fmapX, fmapY, cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0,0,0));

    for (int y = 0; y < H; ++y) {
        const uint8_t* sp = stripFrame.ptr<uint8_t>(y);
        const uint8_t* fp = finalFrame.ptr<uint8_t>(y);
        const float* rowMap = mapY.ptr<float>(y);
        uint8_t* dp = frame.ptr<uint8_t>(y);
        for (int x = 0; x < W; ++x) {
            if (rowMap[x] >= 0.0f) {
                const double blend = rowMap[x] > finalBlendStart
                    ? smoothstep01((static_cast<double>(rowMap[x]) - static_cast<double>(finalBlendStart)) /
                                   static_cast<double>(finalBlendRows))
                    : 0.0;
                dp[3*x+0] = blendByte(sp[3*x+0], fp[3*x+0], blend);
                dp[3*x+1] = blendByte(sp[3*x+1], fp[3*x+1], blend);
                dp[3*x+2] = blendByte(sp[3*x+2], fp[3*x+2], blend);
            } else {
                dp[3*x+0] = fp[3*x+0];
                dp[3*x+1] = fp[3*x+1];
                dp[3*x+2] = fp[3*x+2];
            }
        }
    }
}

uint8_t clampByte(double v) {
    if (v <= 0.0) return 0;
    if (v >= 255.0) return 255;
    return static_cast<uint8_t>(v + 0.5);
}

void sampleBgrBilinearBorder(const cv::Mat& img, double x, double y, uint8_t* dst) {
    double b = 0.0;
    double g = 0.0;
    double r = 0.0;
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const double fx = x - static_cast<double>(x0);
    const double fy = y - static_cast<double>(y0);

    for (int oy = 0; oy <= 1; ++oy) {
        const int sy = y0 + oy;
        if (sy < 0 || sy >= img.rows) continue;
        const double wy = oy == 0 ? (1.0 - fy) : fy;
        const uint8_t* row = img.ptr<uint8_t>(sy);
        for (int ox = 0; ox <= 1; ++ox) {
            const int sx = x0 + ox;
            if (sx < 0 || sx >= img.cols) continue;
            const double wx = ox == 0 ? (1.0 - fx) : fx;
            const double w = wx * wy;
            const uint8_t* px = row + 3 * sx;
            b += w * static_cast<double>(px[0]);
            g += w * static_cast<double>(px[1]);
            r += w * static_cast<double>(px[2]);
        }
    }

    dst[0] = clampByte(b);
    dst[1] = clampByte(g);
    dst[2] = clampByte(r);
}

bool videoColormapWrapsPhase(compute::Colormap colormap) {
    return colormap == compute::Colormap::ClassicCos ||
           colormap == compute::Colormap::HsvWheel ||
           colormap == compute::Colormap::Tri765 ||
           colormap == compute::Colormap::Twilight ||
           colormap == compute::Colormap::Spectral1530;
}

double applyVideoDepthPhase(double mapped, double phase, compute::Colormap colormap) {
    mapped = std::clamp(mapped, 0.0, 1.0);
    if (colormap == compute::Colormap::Grayscale || colormap == compute::Colormap::Mod17) {
        return mapped;
    }
    if (videoColormapWrapsPhase(colormap)) {
        return std::fmod(mapped + phase, 1.0);
    }
    return std::clamp(mapped + phase * (1.0 - mapped), 0.0, 1.0);
}

double finalFrameAbs2At(const compute::MapParams& p, int x, int y) {
    const double aspect = static_cast<double>(p.width) / static_cast<double>(p.height);
    const double spanIm = p.scale;
    const double spanRe = p.scale * aspect;
    const double reMin = p.center_re - spanRe * 0.5;
    const double imMax = p.center_im + spanIm * 0.5;
    const double re = reMin + (static_cast<double>(x) + 0.5) / static_cast<double>(p.width) * spanRe;
    const double im = imMax - (static_cast<double>(y) + 0.5) / static_cast<double>(p.height) * spanIm;
    return re * re + im * im;
}

void colorizeFinalFrameWithLnMapMode(
    const compute::MapParams& p,
    const compute::FieldOutput& field,
    const std::string& mode,
    const compute::LnMapEqualization& eq,
    cv::Mat& out
) {
    if (field.metric != compute::Metric::Escape ||
        field.width != p.width || field.height != p.height ||
        field.iter_u32.size() != static_cast<size_t>(p.width) * static_cast<size_t>(p.height)) {
        throw std::runtime_error("invalid final-frame escape field");
    }
    if (out.empty() || out.rows != p.height || out.cols != p.width || out.type() != CV_8UC3) {
        out.create(p.height, p.width, CV_8UC3);
    }

    // hist_eq reuses the strip's shared equalization so the warp blend is seamless;
    // every escaped pixel is then a pure function of its iteration count.
    const bool usePeriodicEq = mode == "hist_eq" && eq.valid;
    if (usePeriodicEq) {
        for (int y = 0; y < p.height; ++y) {
            uint8_t* row = out.ptr<uint8_t>(y);
            const size_t rowOffset = static_cast<size_t>(y) * static_cast<size_t>(p.width);
            for (int x = 0; x < p.width; ++x) {
                const int it = static_cast<int>(field.iter_u32[rowOffset + static_cast<size_t>(x)]);
                uint8_t* px = row + 3 * x;
                if (it >= p.iterations) { px[0] = px[1] = px[2] = 255; continue; }
                eq.colorize(it, px[0], px[1], px[2]);
            }
        }
        return;
    }

    const bool needsGlobalCdf = mode == "hist_eq" || mode == "bands" || mode == "frontier";
    std::vector<unsigned long long> hist;
    unsigned long long total = 0;
    int firstHistIter = p.iterations;
    if (needsGlobalCdf) {
        hist.assign(static_cast<size_t>(p.iterations), 0ULL);
        for (int y = 0; y < p.height; ++y) {
            const size_t rowOffset = static_cast<size_t>(y) * static_cast<size_t>(p.width);
            for (int x = 0; x < p.width; ++x) {
                if (finalFrameAbs2At(p, x, y) > 4.0) continue;
                const int it = static_cast<int>(field.iter_u32[rowOffset + static_cast<size_t>(x)]);
                if (it >= 0 && it < p.iterations) {
                    hist[static_cast<size_t>(it)] += 1ULL;
                    total += 1ULL;
                }
            }
        }
        for (int i = 0; i < p.iterations; ++i) {
            if (hist[static_cast<size_t>(i)] > 0ULL) {
                firstHistIter = i;
                break;
            }
        }
        unsigned long long cumulative = 0;
        for (auto& count : hist) {
            cumulative += count;
            count = cumulative;
        }
    }

    auto rawQ = [&](int it) {
        const double q = (static_cast<double>(it) + 1.0) / (static_cast<double>(p.iterations) + 1.0);
        return std::clamp(q, 0.0, 1.0);
    };
    auto contextQ = [&](int it) {
        const double raw = rawQ(it);
        return std::log1p(48.0 * raw) / std::log1p(48.0);
    };
    auto globalQ = [&](int it) {
        if (total > 0 && it >= 0 && it < p.iterations && !hist.empty()) {
            const double q = std::clamp(
                static_cast<double>(hist[static_cast<size_t>(it)]) / static_cast<double>(total),
                0.0,
                1.0);
            if (q <= 0.0 && firstHistIter < p.iterations && it < firstHistIter) {
                const double denom = std::max(1.0e-12, rawQ(firstHistIter));
                return 0.10 * std::clamp(rawQ(it) / denom, 0.0, 1.0);
            }
            return 0.10 + 0.90 * q;
        }
        return rawQ(it);
    };
    auto qAt = [&](int y, int x) {
        y = std::clamp(y, 0, p.height - 1);
        x = std::clamp(x, 0, p.width - 1);
        const int it = static_cast<int>(field.iter_u32[static_cast<size_t>(y) * static_cast<size_t>(p.width) + static_cast<size_t>(x)]);
        if (it >= p.iterations) return 1.0;
        return globalQ(it);
    };

    constexpr double depth01 = 1.0;
    for (int y = 0; y < p.height; ++y) {
        uint8_t* row = out.ptr<uint8_t>(y);
        std::vector<int> rowIters;
        if (mode == "row_eq") {
            rowIters.reserve(static_cast<size_t>(p.width));
            const size_t rowOffset = static_cast<size_t>(y) * static_cast<size_t>(p.width);
            for (int x = 0; x < p.width; ++x) {
                const int it = static_cast<int>(field.iter_u32[rowOffset + static_cast<size_t>(x)]);
                if (it >= 0 && it < p.iterations) rowIters.push_back(it);
            }
            std::sort(rowIters.begin(), rowIters.end());
        }
        auto rowQ = [&](int it) {
            if (!rowIters.empty() && it >= 0 && it < p.iterations) {
                const auto hi = std::upper_bound(rowIters.begin(), rowIters.end(), it);
                return std::clamp(
                    static_cast<double>(hi - rowIters.begin()) / static_cast<double>(rowIters.size()),
                    0.0,
                    1.0);
            }
            return rawQ(it);
        };

        const size_t rowOffset = static_cast<size_t>(y) * static_cast<size_t>(p.width);
        for (int x = 0; x < p.width; ++x) {
            const int it = static_cast<int>(field.iter_u32[rowOffset + static_cast<size_t>(x)]);
            uint8_t* px = row + 3 * x;
            if (it >= p.iterations) {
                px[0] = px[1] = px[2] = 255;
                continue;
            }

            double mapped = 0.0;
            if (mode == "log_lift") {
                mapped = contextQ(it) * (0.82 + 0.18 * depth01);
                mapped = applyVideoDepthPhase(mapped, 0.15 * depth01, p.colormap);
            } else if (mode == "row_eq") {
                mapped = rowQ(it) * (0.86 + 0.14 * depth01);
                mapped = applyVideoDepthPhase(mapped, 0.12 * depth01, p.colormap);
            } else if (mode == "bands") {
                const double base = globalQ(it);
                const double raw = rawQ(it);
                const double broad = 0.5 + 0.5 * std::sin(TAU * (base * 18.0 + depth01 * 0.72));
                const double fine = 0.5 + 0.5 * std::sin(TAU * (std::sqrt(raw) * 72.0 + depth01 * 0.19));
                mapped = std::clamp(0.70 * base + 0.22 * broad + 0.08 * fine, 0.0, 1.0);
                mapped = applyVideoDepthPhase(mapped, 0.08 * depth01, p.colormap);
            } else if (mode == "frontier") {
                const double base = globalQ(it);
                const double dx = qAt(y, x + 1) - qAt(y, x - 1);
                const double dy = qAt(y + 1, x) - qAt(y - 1, x);
                const double edge = std::clamp(std::sqrt(dx * dx + dy * dy) * 5.0, 0.0, 1.0);
                mapped = std::clamp(0.76 * base + 0.24 * edge, 0.0, 1.0);
                mapped = applyVideoDepthPhase(mapped, 0.10 * depth01, p.colormap);
                compute::colorize_field_bgr(mapped, p.colormap, px[0], px[1], px[2]);
                const double lift = 0.34 * edge;
                px[0] = static_cast<uint8_t>(compute::clamp255(static_cast<int>(static_cast<double>(px[0]) * (1.0 - lift) + 255.0 * lift)));
                px[1] = static_cast<uint8_t>(compute::clamp255(static_cast<int>(static_cast<double>(px[1]) * (1.0 - lift) + 255.0 * lift)));
                px[2] = static_cast<uint8_t>(compute::clamp255(static_cast<int>(static_cast<double>(px[2]) * (1.0 - lift) + 255.0 * lift)));
                continue;
            } else {
                const double q = globalQ(it);
                mapped = 0.10 + q * 0.90;
                mapped = applyVideoDepthPhase(mapped, 0.22 * depth01, p.colormap);
            }
            compute::colorize_field_bgr(mapped, p.colormap, px[0], px[1], px[2]);
        }
    }
}

void renderWarpFrameManualCpu(
    const cv::Mat& stripWrap,
    const cv::Mat& finalImg,
    const CpuWarpGeometry& geom,
    int W,
    int H,
    double kTop,
    double kTop_end,
    cv::Mat& frame
) {
    if (frame.empty() || frame.rows != H || frame.cols != W || frame.type() != CV_8UC3) {
        frame.create(H, W, CV_8UC3);
    }

    const int stripH = stripWrap.rows;
    const int s = stripWrap.cols - 1;
    const float stripLimit = static_cast<float>(stripH - 1);
    const float finalBlendRows = std::max(4.0f, static_cast<float>(s) / 128.0f);
    const float finalBlendStart = stripLimit - finalBlendRows;
    const float kTopStripScale = static_cast<float>(kTop * static_cast<double>(s) / TAU);
    const double S = std::exp(kTop - kTop_end);

    for (int y = 0; y < H; ++y) {
        const float* stripX = geom.strip_x.ptr<float>(y);
        const float* rowBase = geom.strip_row_base.ptr<float>(y);
        const double finalY = (static_cast<double>(y) + 0.5 - 0.5 * static_cast<double>(H)) * S
                              + 0.5 * static_cast<double>(H);
        uint8_t* dp = frame.ptr<uint8_t>(y);
        for (int x = 0; x < W; ++x) {
            const float row = rowBase[x] - kTopStripScale;
            const bool useStrip = row >= 0.0f && row < stripLimit;

            uint8_t* out = dp + 3 * x;
            if (useStrip) {
                sampleBgrBilinearBorder(stripWrap, stripX[x], row, out);
                if (row > finalBlendStart) {
                    uint8_t finalPx[3] = {};
                    const double finalX = (static_cast<double>(x) + 0.5 - 0.5 * static_cast<double>(W)) * S
                                          + 0.5 * static_cast<double>(W);
                    sampleBgrBilinearBorder(finalImg, finalX, finalY, finalPx);
                    const double blend = smoothstep01((static_cast<double>(row) - static_cast<double>(finalBlendStart)) /
                                                      static_cast<double>(finalBlendRows));
                    out[0] = blendByte(out[0], finalPx[0], blend);
                    out[1] = blendByte(out[1], finalPx[1], blend);
                    out[2] = blendByte(out[2], finalPx[2], blend);
                }
            } else {
                const double finalX = (static_cast<double>(x) + 0.5 - 0.5 * static_cast<double>(W)) * S
                                      + 0.5 * static_cast<double>(W);
                sampleBgrBilinearBorder(finalImg, finalX, finalY, out);
            }
        }
    }
}

cv::Mat renderZoomPreviewFrameSmart(
    const cv::Mat& strip,
    const cv::Mat& finalImg,
    int W,
    int H,
    double kTop,
    double kTop_end,
    bool preferCudaWarp
) {
    cv::Mat stripWrap;
    cv::copyMakeBorder(strip, stripWrap, 0, 0, 0, 1, cv::BORDER_WRAP);

    cv::Mat frame(H, W, CV_8UC3);
    const bool remapSafe = opencv_remap_size_safe(stripWrap, W, H)
        && opencv_remap_size_safe(finalImg, W, H);

#if USE_CUDA_VIDEO_WARP
    if (preferCudaWarp && fsd_cuda::cuda_video_warp_available()) {
        fsd_cuda::CudaVideoWarpContext cudaWarp;
        try {
            fsd_cuda::cuda_video_warp_init(stripWrap, finalImg, cudaWarp);
            fsd_cuda::cuda_video_warp_frame(cudaWarp, kTop, kTop_end, frame);
            fsd_cuda::cuda_video_warp_release(cudaWarp);
            return frame;
        } catch (...) {
            fsd_cuda::cuda_video_warp_release(cudaWarp);
        }
    }
#else
    (void)preferCudaWarp;
#endif

    const CpuWarpGeometry geom = buildCpuWarpGeometry(W, H, stripWrap.cols, stripWrap.rows);
    if (remapSafe) {
        cv::Mat stripFrame(H, W, CV_8UC3);
        cv::Mat finalFrame(H, W, CV_8UC3);
        cv::Mat mapY(H, W, CV_32FC1);
        cv::Mat fmapX(H, W, CV_32FC1);
        cv::Mat fmapY(H, W, CV_32FC1);
        renderWarpFrameShared(stripWrap, finalImg, geom, W, H, kTop, kTop_end,
                              frame, stripFrame, finalFrame, mapY, fmapX, fmapY);
    } else {
        renderWarpFrameManualCpu(stripWrap, finalImg, geom, W, H, kTop, kTop_end, frame);
    }
    return frame;
}

// ─── Shared video generation helper ──────────────────────────────────────────
static std::string generateZoomVideo(
    const cv::Mat& strip,
    const cv::Mat& finalImg,
    int W, int H, int fps, int frameCount,
    double kTop_start, double kTop_end, double depthOctaves,
    const std::filesystem::path& outDir, const std::string& baseName,
    const std::function<void(int)>& on_frame_done = nullptr,
    std::string* ffmpeg_stderr_out = nullptr,
    std::string* encoder_out = nullptr,
    bool prefer_cuda_warp = true,
    std::string* warp_method_out = nullptr,
    VideoWarpStats* stats_out = nullptr,
    const std::function<bool()>& should_cancel = nullptr
) {
    if (encoder_out) *encoder_out = "";

    cv::Mat stripWrap;
    cv::copyMakeBorder(strip, stripWrap, 0, 0, 0, 1, cv::BORDER_WRAP);

    const bool remapSafe = opencv_remap_size_safe(stripWrap, W, H)
        && opencv_remap_size_safe(finalImg, W, H);
    VideoWarpStats baseStats;
    baseStats.frameCount = frameCount;
    baseStats.stripWidth = strip.cols;
    baseStats.stripHeight = strip.rows;
    baseStats.opencvRemapSafe = remapSafe;
    std::string selectedWarpMethod;

#if USE_CUDA_VIDEO_WARP
    fsd_cuda::CudaVideoWarpContext cudaWarp;
    struct CudaWarpGuard {
        fsd_cuda::CudaVideoWarpContext* ctx = nullptr;
        ~CudaWarpGuard() { if (ctx) fsd_cuda::cuda_video_warp_release(*ctx); }
    } cudaWarpGuard{&cudaWarp};
    bool useCudaWarp = false;
    if (prefer_cuda_warp && fsd_cuda::cuda_video_warp_available()) {
        try {
            fsd_cuda::cuda_video_warp_init(stripWrap, finalImg, cudaWarp);
            useCudaWarp = true;
            selectedWarpMethod = "cuda_texture";
        } catch (...) {
            fsd_cuda::cuda_video_warp_release(cudaWarp);
            useCudaWarp = false;
        }
    }
#else
    (void)prefer_cuda_warp;
#endif
    if (selectedWarpMethod.empty()) {
        selectedWarpMethod = remapSafe ? "opencv_cpu_remap_precomputed" : "manual_cpu_bilinear_precomputed";
    }
    baseStats.warpMethod = selectedWarpMethod;
    if (warp_method_out) *warp_method_out = selectedWarpMethod;

    const std::filesystem::path mp4 = outDir / (baseName + ".mp4");
    const std::filesystem::path avi = outDir / (baseName + ".avi");
    const std::filesystem::path tmpMp4 = outDir / (baseName + ".tmp.mp4");
    const std::filesystem::path tmpAvi = outDir / (baseName + ".tmp.avi");

    cv::Mat frame;
    cv::Mat stripFrame;
    cv::Mat finalFrame;
    cv::Mat mapY;
    cv::Mat fmapX;
    cv::Mat fmapY;
    std::unique_ptr<CpuWarpGeometry> cpuGeom;
    auto getCpuGeom = [&]() -> const CpuWarpGeometry& {
        if (!cpuGeom) {
            cpuGeom = std::make_unique<CpuWarpGeometry>(
                buildCpuWarpGeometry(W, H, stripWrap.cols, stripWrap.rows));
        }
        return *cpuGeom;
    };

    auto renderCudaFramesAsync = [&](auto&& writeFrame, VideoWarpStats& stats) -> bool {
#if USE_CUDA_VIDEO_WARP
        if (!useCudaWarp) return false;
        const size_t frameBytes = static_cast<size_t>(W) * static_cast<size_t>(H) * 3u;

        struct PinnedFrames {
            void* ptr[2] = {nullptr, nullptr};
            ~PinnedFrames() {
                fsd_cuda::cuda_video_warp_free_pinned(ptr[0]);
                fsd_cuda::cuda_video_warp_free_pinned(ptr[1]);
            }
        } pinned;

        try {
            pinned.ptr[0] = fsd_cuda::cuda_video_warp_alloc_pinned(frameBytes);
            pinned.ptr[1] = fsd_cuda::cuda_video_warp_alloc_pinned(frameBytes);
        } catch (...) {
            return false;
        }

        cv::Mat hostFrame[2] = {
            cv::Mat(H, W, CV_8UC3, pinned.ptr[0]),
            cv::Mat(H, W, CV_8UC3, pinned.ptr[1]),
        };
        bool pending[2] = {false, false};

        auto scheduleFrame = [&](int f) {
            if (should_cancel && should_cancel()) throw std::runtime_error("cancelled");
            const double tNorm = static_cast<double>(f) / std::max(1, frameCount - 1);
            const double kTop  = kTop_start - tNorm * depthOctaves * LN_TWO;
            const int buffer = f & 1;
            fsd_cuda::cuda_video_warp_frame_async(cudaWarp, kTop, kTop_end, buffer, pinned.ptr[buffer]);
            pending[buffer] = true;
        };

        auto finishFrame = [&](int f) {
            const int buffer = f & 1;
            fsd_cuda::CudaVideoWarpTiming timing;
            fsd_cuda::cuda_video_warp_wait_frame(cudaWarp, buffer, &timing);
            pending[buffer] = false;
            stats.warpTotalMs += timing.kernel_ms;
            stats.copyTotalMs += timing.copy_ms;

            const auto writeStart = std::chrono::steady_clock::now();
            writeFrame(hostFrame[buffer]);
            const auto writeEnd = std::chrono::steady_clock::now();
            stats.writeTotalMs += std::chrono::duration<double, std::milli>(writeEnd - writeStart).count();
            stats.rawVideoBytes += static_cast<uint64_t>(frameBytes);
            if (should_cancel && should_cancel()) throw std::runtime_error("cancelled");
            if (on_frame_done && (f + 1 == frameCount || ((f + 1) % 8) == 0)) on_frame_done(f + 1);
        };

        try {
            scheduleFrame(0);
            for (int f = 1; f < frameCount; ++f) {
                scheduleFrame(f);
                finishFrame(f - 1);
            }
            finishFrame(frameCount - 1);
        } catch (...) {
            for (int i = 0; i < 2; ++i) {
                if (!pending[i]) continue;
                try { fsd_cuda::cuda_video_warp_wait_frame(cudaWarp, i, nullptr); } catch (...) {}
            }
            throw;
        }
        return true;
#else
        (void)writeFrame;
        (void)stats;
        return false;
#endif
    };

    auto renderFrames = [&](auto&& writeFrame, VideoWarpStats& stats) {
        if (renderCudaFramesAsync(writeFrame, stats)) return;

        for (int f = 0; f < frameCount; f++) {
            if (should_cancel && should_cancel()) throw std::runtime_error("cancelled");
            const double tNorm = static_cast<double>(f) / std::max(1, frameCount - 1);
            const double kTop  = kTop_start - tNorm * depthOctaves * LN_TWO;
#if USE_CUDA_VIDEO_WARP
            if (useCudaWarp) {
                fsd_cuda::CudaVideoWarpTiming timing;
                fsd_cuda::cuda_video_warp_frame_timed(cudaWarp, kTop, kTop_end, frame, &timing);
                stats.warpTotalMs += timing.kernel_ms;
                stats.copyTotalMs += timing.copy_ms;
            } else
#endif
            {
                const auto warpStart = std::chrono::steady_clock::now();
                const CpuWarpGeometry& geom = getCpuGeom();
                if (selectedWarpMethod == "opencv_cpu_remap_precomputed") {
                    ensureMat(frame, W, H, CV_8UC3);
                    ensureMat(stripFrame, W, H, CV_8UC3);
                    ensureMat(finalFrame, W, H, CV_8UC3);
                    ensureMat(mapY, W, H, CV_32FC1);
                    ensureMat(fmapX, W, H, CV_32FC1);
                    ensureMat(fmapY, W, H, CV_32FC1);
                    renderWarpFrameShared(stripWrap, finalImg, geom, W, H, kTop, kTop_end,
                                          frame, stripFrame, finalFrame, mapY, fmapX, fmapY);
                } else {
                    renderWarpFrameManualCpu(stripWrap, finalImg, geom, W, H, kTop, kTop_end, frame);
                }
                const auto warpEnd = std::chrono::steady_clock::now();
                stats.warpTotalMs += std::chrono::duration<double, std::milli>(warpEnd - warpStart).count();
            }
            const size_t bytes = static_cast<size_t>(frame.rows) * frame.step;
            const auto writeStart = std::chrono::steady_clock::now();
            writeFrame(frame);
            const auto writeEnd = std::chrono::steady_clock::now();
            stats.writeTotalMs += std::chrono::duration<double, std::milli>(writeEnd - writeStart).count();
            stats.rawVideoBytes += static_cast<uint64_t>(bytes);
            if (should_cancel && should_cancel()) throw std::runtime_error("cancelled");
            if (on_frame_done && (f + 1 == frameCount || ((f + 1) % 8) == 0)) on_frame_done(f + 1);
        }
    };

    const std::filesystem::path ffmpegErr = outDir / (baseName + "_ffmpeg.stderr.txt");
    const std::filesystem::path ffmpegErrTmp = outDir / (baseName + "_ffmpeg.stderr.txt.tmp");
    std::vector<std::pair<std::string, std::string>> ffmpegCmds;
    const std::string inputArgs =
        "ffmpeg -y -f rawvideo -pix_fmt bgr24 -s " + std::to_string(W) + "x" + std::to_string(H) +
        " -r " + std::to_string(fps) + " -i - -an ";
    if (commandSucceeds("bash -lc \"ffmpeg -hide_banner -encoders 2>/dev/null | grep -q h264_nvenc\"")) {
        ffmpegCmds.push_back({"h264_nvenc",
            inputArgs + "-c:v h264_nvenc -pix_fmt yuv420p -preset p5 -cq 18 \"" + tmpMp4.string() +
            "\" 2>\"" + ffmpegErrTmp.string() + "\""});
    }
    if (commandSucceeds("bash -lc \"ffmpeg -hide_banner -encoders 2>/dev/null | grep -q hevc_nvenc\"")) {
        ffmpegCmds.push_back({"hevc_nvenc",
            inputArgs + "-c:v hevc_nvenc -pix_fmt yuv420p -preset p5 -cq 20 \"" + tmpMp4.string() +
            "\" 2>\"" + ffmpegErrTmp.string() + "\""});
    }
    ffmpegCmds.push_back({"libx264",
        inputArgs + "-c:v libx264 -pix_fmt yuv420p -preset medium -crf 16 \"" + tmpMp4.string() +
        "\" 2>\"" + ffmpegErrTmp.string() + "\""});

    for (const auto& [encoderName, ffmpegCmd] : ffmpegCmds) {
        if (FILE* pipe = popen(ffmpegCmd.c_str(), "w")) {
            bool ok = true;
            VideoWarpStats attemptStats = baseStats;
            attemptStats.encoder = encoderName;
            try {
                renderFrames([&](const cv::Mat& rendered) {
                    const size_t bytes = static_cast<size_t>(rendered.rows) * rendered.step;
                    if (std::fwrite(rendered.data, 1, bytes, pipe) != bytes) {
                        ok = false;
                        throw std::runtime_error("ffmpeg pipe write failed");
                    }
                }, attemptStats);
            } catch (const std::exception& e) {
                pclose(pipe);
                std::error_code ec;
                std::filesystem::remove(tmpMp4, ec);
                std::filesystem::remove(tmpAvi, ec);
                std::filesystem::remove(ffmpegErrTmp, ec);
                if (std::string(e.what()) == "ffmpeg pipe write failed") {
                    continue;
                }
                throw;
            } catch (...) {
                pclose(pipe);
                std::error_code ec;
                std::filesystem::remove(tmpMp4, ec);
                std::filesystem::remove(tmpAvi, ec);
                std::filesystem::remove(ffmpegErrTmp, ec);
                throw;
            }
            const auto closeStart = std::chrono::steady_clock::now();
            const int rc = pclose(pipe);
            const auto closeEnd = std::chrono::steady_clock::now();
            attemptStats.encodeCloseMs = std::chrono::duration<double, std::milli>(closeEnd - closeStart).count();
            if (ffmpeg_stderr_out) {
                std::ifstream errIn(ffmpegErrTmp);
                std::ostringstream ss; ss << errIn.rdbuf();
                *ffmpeg_stderr_out = ss.str();
            }
            if (std::filesystem::exists(ffmpegErrTmp)) {
                std::error_code ec;
                std::filesystem::rename(ffmpegErrTmp, ffmpegErr, ec);
                if (ec) {
                    std::filesystem::remove(ffmpegErr, ec);
                    ec.clear();
                    std::filesystem::rename(ffmpegErrTmp, ffmpegErr, ec);
                }
            }
            if (ok && rc == 0 && std::filesystem::exists(tmpMp4)) {
                std::error_code ec;
                std::filesystem::rename(tmpMp4, mp4, ec);
                if (ec) {
                    std::filesystem::remove(mp4, ec);
                    ec.clear();
                    std::filesystem::rename(tmpMp4, mp4, ec);
                }
                if (encoder_out) *encoder_out = encoderName;
                if (stats_out) *stats_out = attemptStats;
                return mp4.string();
            }
        }
    }

    std::error_code removeEc;
    std::filesystem::remove(mp4, removeEc);
    const int fourcc = cv::VideoWriter::fourcc('m', 'p', '4', 'v');
    cv::VideoWriter writer(tmpMp4.string(), fourcc, static_cast<double>(fps), cv::Size(W, H), true);
    std::string encoderName = "VideoWriter:mp4v";
    if (!writer.isOpened()) {
        writer.open(tmpAvi.string(), cv::VideoWriter::fourcc('M','J','P','G'), static_cast<double>(fps), cv::Size(W, H), true);
        encoderName = "VideoWriter:MJPG";
        if (!writer.isOpened()) {
            std::string msg = "VideoWriter failed";
            if (ffmpeg_stderr_out && !ffmpeg_stderr_out->empty()) {
                msg += "; ffmpeg stderr: " + *ffmpeg_stderr_out;
            }
            throw std::runtime_error(msg);
        }
    }

    VideoWarpStats writerStats = baseStats;
    writerStats.encoder = encoderName;
    try {
        renderFrames([&](const cv::Mat& rendered) {
            writer.write(rendered);
        }, writerStats);
        const auto closeStart = std::chrono::steady_clock::now();
        writer.release();
        const auto closeEnd = std::chrono::steady_clock::now();
        writerStats.encodeCloseMs = std::chrono::duration<double, std::milli>(closeEnd - closeStart).count();
    } catch (...) {
        writer.release();
        std::error_code ec;
        std::filesystem::remove(tmpMp4, ec);
        std::filesystem::remove(tmpAvi, ec);
        throw;
    }

    if (std::filesystem::exists(tmpMp4)) {
        std::error_code ec;
        std::filesystem::rename(tmpMp4, mp4, ec);
        if (ec) {
            std::filesystem::remove(mp4, ec);
            ec.clear();
            std::filesystem::rename(tmpMp4, mp4, ec);
        }
        if (encoder_out) *encoder_out = encoderName;
        if (stats_out) *stats_out = writerStats;
        return mp4.string();
    }
    if (std::filesystem::exists(tmpAvi)) {
        std::error_code ec;
        std::filesystem::rename(tmpAvi, avi, ec);
        if (ec) {
            std::filesystem::remove(avi, ec);
            ec.clear();
            std::filesystem::rename(tmpAvi, avi, ec);
        }
        if (encoder_out) *encoder_out = encoderName;
        if (stats_out) *stats_out = writerStats;
        return avi.string();
    }
    return mp4.string();
}

struct LnMapLookup {
    std::filesystem::path pngPath;
    Json sidecar;
};

LnMapLookup resolveLnMap(const std::filesystem::path& repoRoot, const std::string& artifactId) {
    namespace fs = std::filesystem;
    const auto split = artifactId.find(':');
    if (split == std::string::npos) throw std::runtime_error("bad artifactId");
    const std::string runId   = artifactId.substr(0, split);
    const std::string fileName = artifactId.substr(split + 1);

    const fs::path runDir = repoRoot / "fractal_studio" / "runtime" / "runs" / runId;
    const fs::path png    = runDir / fileName;
    if (!fs::exists(png)) throw std::runtime_error("ln-map png not found: " + png.string());

    fs::path sidecar = runDir / "ln_map.json";
    if (!fs::exists(sidecar)) throw std::runtime_error("ln-map sidecar not found");

    std::ifstream in(sidecar);
    std::ostringstream ss; ss << in.rdbuf();
    return { png, Json::parse(ss.str()) };
}

struct VideoEtaState {
    std::string stage;
    int total = 0;
    int last_current = 0;
    long long stage_started_ms = 0;
};

std::mutex g_video_eta_mu;
std::unordered_map<std::string, VideoEtaState> g_video_eta;

long long etaClockMs() {
    static const auto origin = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - origin).count();
}

long long estimateStageRemainingMs(const std::string& runId, const std::string& stage, int current, int total) {
    const long long now = etaClockMs();
    const bool terminal = stage == "failed" || stage == "cancelled";
    std::lock_guard<std::mutex> lk(g_video_eta_mu);
    if (terminal) {
        g_video_eta.erase(runId);
        return -1;
    }

    auto& state = g_video_eta[runId];
    if (state.stage != stage || state.total != total || current < state.last_current) {
        state.stage = stage;
        state.total = total;
        state.last_current = std::max(0, current);
        state.stage_started_ms = now;
    } else {
        state.last_current = std::max(state.last_current, current);
    }

    if (total <= 0 || current <= 0) return -1;
    if (current >= total) {
        if (stage == "video_warp_encode") g_video_eta.erase(runId);
        return 0;
    }

    const long long elapsed = std::max(1LL, now - state.stage_started_ms);
    if (elapsed < 1000) return -1;
    return static_cast<long long>(
        std::ceil(static_cast<double>(elapsed) *
                  static_cast<double>(total - current) /
                  static_cast<double>(current)));
}

void setVideoProgress(
    JobRunner& runner,
    const std::string& runId,
    const std::string& stage,
    int current,
    int total,
    double depthCurrent,
    double depthTotal,
    const std::string& failedStage = "",
    const std::string& errorMessage = "",
    Json details = Json::object()
) {
    const double percent = total > 0 ? (100.0 * static_cast<double>(current) / static_cast<double>(total)) : 0.0;
    const long long elapsedMs = runner.runElapsedMs(runId);
    const long long estimatedRemainingMs = estimateStageRemainingMs(runId, stage, current, total);
    Json j = {
        {"taskType", "video_export"},
        {"stage", stage},
        {"current", current},
        {"total", total},
        {"percent", percent},
        {"elapsedMs", elapsedMs},
        {"estimatedRemainingMs", estimatedRemainingMs >= 0 ? Json(estimatedRemainingMs) : Json(nullptr)},
        {"cancelable", true},
        {"resourceLocks", Json::array({"video_export", "cuda_heavy", "cpu_heavy"})},
        {"depthOctave", depthCurrent},
        {"totalDepthOctaves", depthTotal},
        {"failedStage", failedStage},
        {"errorMessage", errorMessage},
        {"details", details},
    };
    for (const char* key : {"engine", "scalar", "finalFrameEngine", "finalFrameScalar", "lnMapEngine", "lnMapScalar", "lnMapMode", "lnMapColorMode", "lnMapLayerSummary", "lnMapValidationSummary", "warpMethod", "encoder", "currentFrame", "totalFrames", "currentLnMapRow", "totalLnMapRows", "warpTotalMs", "copyTotalMs", "writeTotalMs", "encodeCloseMs", "avgWarpMs", "avgCopyMs", "avgWriteMs", "rawVideoBytes", "stripWidth", "stripHeight", "opencvRemapSafe"}) {
        if (details.contains(key)) j[key] = details[key];
    }
    runner.setProgress(runId, j.dump());
}

void throwIfCancelled(JobRunner& runner, const std::string& runId) {
    if (runner.isCancelRequested(runId)) throw std::runtime_error("cancelled");
}

} // namespace

std::string zoomVideoRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& body) {
    const Json j = parseJsonBody(body);
    const std::string lnArtifactId = j.value("lnMapArtifactId", std::string(""));
    if (lnArtifactId.empty()) throw std::runtime_error("lnMapArtifactId required");

    const int    fps         = j.value("fps",    30);
    const int    W           = j.value("width",  720);
    const int    H           = j.value("height", 720);
    const bool   localExport = j.value("localExport", false);

    if (fps < 1 || fps > 120) throw std::runtime_error("invalid fps (1..120)");
    validateVideoOutputSize(W, H);

    LnMapLookup lk = resolveLnMap(repoRoot, lnArtifactId);
    cv::Mat strip  = compute::read_png(lk.pngPath.string());

    const double sidecarDepth = lk.sidecar.value("depthOctaves", 40.0);
    const double cr           = lk.sidecar.value("centerRe",    0.0);
    const double ci           = lk.sidecar.value("centerIm",    0.0);
    const int    iters        = lk.sidecar.value("iterations",  4096);
    const std::string variantStr  = lk.sidecar.value("variant",  std::string("mandelbrot"));
    const std::string colormapStr = lk.sidecar.value("colorMap", std::string("classic_cos"));

    double depthOctaves = resolveDepthOctaves(j, kTopStartForFrame(W, H), sidecarDepth - 1.5);
    if (depthOctaves < 0.05 || depthOctaves > 120.0) {
        throw std::runtime_error("invalid depthOctaves (0.05..120)");
    }
    const double secondsPerOctave = resolveSecondsPerOctave(j, depthOctaves);
    double durationSec = 0.0;
    const int frameCount = frameCountFromSpeed(depthOctaves, secondsPerOctave, fps, durationSec);

    // ── startKTop: corner pixel of first frame hits strip row 0 ──────────────
    const double kTop_start = kTopStartForFrame(W, H);
    const double kTop_end   = kTop_start - depthOctaves * LN_TWO;

    // ── Render final cartesian image at kTop_end ──────────────────────────────
    // This fills the centre pixels that the ln-map strip can't reach.
    compute::Variant variantVal;
    if (!compute::variant_from_name(variantStr.c_str(), variantVal))
        variantVal = compute::Variant::Mandelbrot;
    double bailout = lk.sidecar.contains("bailout") && !lk.sidecar["bailout"].is_null()
        ? lk.sidecar.value("bailout", 2.0)
        : compute::variant_default_bailout(variantVal);
    const double bailoutSq = lk.sidecar.contains("bailoutSq") && !lk.sidecar["bailoutSq"].is_null()
        ? lk.sidecar.value("bailoutSq", compute::variant_default_bailout_sq(variantVal))
        : (lk.sidecar.contains("bailout") && !lk.sidecar["bailout"].is_null()
            ? bailout * bailout
            : compute::variant_default_bailout_sq(variantVal));
    if (lk.sidecar.contains("bailoutSq") && !lk.sidecar["bailoutSq"].is_null() &&
        !(lk.sidecar.contains("bailout") && !lk.sidecar["bailout"].is_null())) {
        bailout = std::sqrt(bailoutSq);
    }
    if (!(bailout > 0.0) || !std::isfinite(bailout)) throw std::runtime_error("invalid bailout");
    if (!(bailoutSq > 0.0) || !std::isfinite(bailoutSq)) throw std::runtime_error("invalid bailoutSq");
    compute::Colormap cmVal;
    if (!compute::colormap_from_name(colormapStr.c_str(), cmVal))
        cmVal = compute::Colormap::ClassicCos;

    // Scale: height of the cartesian view at kTop_end in complex units.
    // A pixel at normalised v=1 is at world height e^kTop_end, so full height = 2·e^kTop_end.
    compute::MapParams mp;
    mp.center_re  = cr;
    mp.center_im  = ci;
    mp.scale      = 2.0 * std::exp(kTop_end);
    mp.width      = W;
    mp.height     = H;
    mp.iterations = iters;
    mp.bailout    = bailout;
    mp.bailout_sq = bailoutSq;
    mp.variant    = variantVal;
    mp.metric     = compute::Metric::Escape;
    mp.colormap   = cmVal;
    mp.smooth     = false;
    mp.engine     = "auto";
    mp.scalar_type = "auto";

    auto run = runner.createRun("zoom-video", body);
    ResourceManager::Lease videoLeaseRaw;
    std::string conflictLock, activeRunId;
    if (!resourceManager().tryAcquire(run.id, "video_export", {"video_export", "cuda_heavy", "cpu_heavy"}, videoLeaseRaw, conflictLock, activeRunId)) {
        runner.setStatus(run.id, "failed");
        throw HttpError(409, Json{
            {"error", "video_export already running"},
            {"activeRunId", activeRunId},
            {"taskType", "video_export"},
            {"resourceLock", conflictLock},
        }.dump());
    }
    auto videoLease = std::make_shared<ResourceManager::Lease>(std::move(videoLeaseRaw));
    (void)videoLease;
    runner.setStatus(run.id, "running");
    runner.setCancelable(run.id, true);

    // ── Video writer ──────────────────────────────────────────────────────────
    cv::Mat finalImg(H, W, CV_8UC3);
    std::string mp4Path;
    std::string ffmpegStderr;
    std::string encoderUsed;
    std::string warpMethod;
    VideoWarpStats warpStats;
    double elapsed = 0.0;

    try {
        const auto t0 = std::chrono::steady_clock::now();
        throwIfCancelled(runner, run.id);
        compute::render_map(mp, finalImg);
        throwIfCancelled(runner, run.id);
        setVideoProgress(runner, run.id, "video_warp_encode", 0, frameCount, depthOctaves, depthOctaves,
                         "", "", Json{{"currentFrame", 0}, {"totalFrames", frameCount}});
        mp4Path = generateZoomVideo(
            strip, finalImg, W, H, fps, frameCount,
            kTop_start, kTop_end, depthOctaves,
            std::filesystem::path(run.outputDir), "zoom",
            [&](int frameDone) {
                setVideoProgress(runner, run.id, "video_warp_encode", frameDone, frameCount, depthOctaves, depthOctaves,
                                 "", "", Json{{"currentFrame", frameDone}, {"totalFrames", frameCount}});
            },
            &ffmpegStderr,
            &encoderUsed,
            j.value("cudaWarp", true),
            &warpMethod,
            &warpStats,
            [&]() { return runner.isCancelRequested(run.id); }
        );
        const auto t1 = std::chrono::steady_clock::now();
        elapsed = std::chrono::duration<double, std::milli>(t1 - t0).count();
        Json doneDetails = {{"currentFrame", frameCount}, {"totalFrames", frameCount}, {"warpMethod", warpMethod}, {"encoder", encoderUsed}};
        mergeVideoWarpStats(doneDetails, warpStats);
        setVideoProgress(runner, run.id, "video_warp_encode", frameCount, frameCount, depthOctaves, depthOctaves,
                         "", "", doneDetails);

        runner.addArtifact(run.id, Artifact{"zoom-video", mp4Path, "video"});
        runner.setStatus(run.id, "completed");
    } catch (const std::exception& e) {
        if (runner.isCancelRequested(run.id) || std::string(e.what()) == "cancelled") {
            setVideoProgress(runner, run.id, "cancelled", 0, frameCount, 0.0, depthOctaves, "video_export", "cancelled");
            runner.setStatus(run.id, "cancelled");
        } else {
            runner.setStatus(run.id, "failed");
        }
        throw;
    }

    const std::string fname = std::filesystem::path(mp4Path).filename().string();
    const std::string artifactId = run.id + ":" + fname;
    Json resp = {
        {"runId",       run.id},
        {"status",      "completed"},
        {"artifactId",  artifactId},
        {"videoUrl",    "/api/artifacts/content?artifactId=" + artifactId},
        {"downloadUrl", "/api/artifacts/download?artifactId=" + artifactId},
        {"localPath",   mp4Path},
        {"localExport", localExport},
        {"frameCount",  frameCount},
        {"fps",         fps},
        {"durationSec", durationSec},
        {"secondsPerOctave", secondsPerOctave},
        {"width",       W},
        {"height",      H},
        {"kTopStart",   kTop_start},
        {"kTopEnd",     kTop_end},
        {"depthOctaves",depthOctaves},
        {"warpMethod",  warpMethod},
        {"encoder",     encoderUsed},
        {"ffmpegStderr",ffmpegStderr},
        {"generatedMs", elapsed},
    };
    mergeVideoWarpStats(resp, warpStats);
    return resp.dump();
}

// ─── Fast preview: direct-render start/end frames before video export ─────────
//
// This intentionally does not build the ln-map strip or encode video. It renders
// the first and final views at preview resolution, so the UI can tune depth
// before paying the full export cost.

std::string videoPreviewRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& body) {
    (void)repoRoot;
    const Json j = parseJsonBody(body);

    const double cr         = j.value("centerRe", 0.0);
    const double ci         = j.value("centerIm", 0.0);
    const bool   julia      = j.value("julia",    false);
    const double jre        = j.value("juliaRe",  0.0);
    const double jim        = j.value("juliaIm",  0.0);
    const std::string variantStr  = j.value("variant",  std::string("mandelbrot"));
    const std::string colormapStr = j.value("colorMap", std::string("classic_cos"));
    const int    iters      = j.value("iterations", 2048);
    const int    fps        = j.value("fps",       30);
    const int    W          = j.value("width",     720);
    const int    H          = j.value("height",    720);

    if (fps < 1 || fps > 120) throw std::runtime_error("invalid fps (1..120)");
    if (iters < 1 || iters > 10000000) throw std::runtime_error("invalid iterations");
    validateVideoOutputSize(W, H);
    const auto [previewW, previewH] = resolvePreviewSize(j, W, H);

    const double kTop_start = kTopStartForFrame(W, H);
    const double depth      = resolveDepthOctaves(j, kTop_start, 20.0);
    if (depth < 0.05 || depth > 120.0) throw std::runtime_error("invalid depthOctaves (0.05..120)");
    const double kTop_end   = kTop_start - depth * LN_TWO;
    const double secondsPerOctave = resolveSecondsPerOctave(j, depth);
    double durSec = 0.0;
    const int frameCount = frameCountFromSpeed(depth, secondsPerOctave, fps, durSec);

    compute::Variant v;
    if (!compute::variant_from_name(variantStr.c_str(), v)) v = compute::Variant::Mandelbrot;
    double bailout = j.contains("bailout") && !j["bailout"].is_null()
        ? j.value("bailout", 2.0)
        : compute::variant_default_bailout(v);
    const double bailoutSq = bailoutSqFromJson(j, bailout, compute::variant_default_bailout_sq(v));
    if (j.contains("bailoutSq") && !j["bailoutSq"].is_null() &&
        !(j.contains("bailout") && !j["bailout"].is_null())) {
        bailout = std::sqrt(bailoutSq);
    }
    if (!(bailout > 0.0) || !std::isfinite(bailout)) throw std::runtime_error("invalid bailout");
    if (!(bailoutSq > 0.0) || !std::isfinite(bailoutSq)) throw std::runtime_error("invalid bailoutSq");
    compute::Colormap cm;
    if (!compute::colormap_from_name(colormapStr.c_str(), cm)) cm = compute::Colormap::ClassicCos;

    auto run = runner.createRun("video-preview", body);
    runner.setStatus(run.id, "running");

    try {
        const auto t0 = std::chrono::steady_clock::now();

        auto renderPreview = [&](double kTop) {
            compute::MapParams mp;
            mp.center_re  = cr;
            mp.center_im  = ci;
            mp.scale      = 2.0 * std::exp(kTop);
            mp.width      = previewW;
            mp.height     = previewH;
            mp.iterations = iters;
            mp.bailout    = bailout;
            mp.bailout_sq = bailoutSq;
            mp.variant    = v;
            mp.metric     = compute::Metric::Escape;
            mp.colormap   = cm;
            mp.smooth     = false;
            mp.julia      = julia;
            mp.julia_re   = jre;
            mp.julia_im   = jim;
            mp.engine     = "auto";
            mp.scalar_type = "auto";

            cv::Mat img(previewH, previewW, CV_8UC3);
            compute::render_map(mp, img);
            return img;
        };

        const cv::Mat startPreview = renderPreview(kTop_start);
        const cv::Mat endPreview   = renderPreview(kTop_end);

        const std::filesystem::path startPath = std::filesystem::path(run.outputDir) / "start_frame.png";
        const std::filesystem::path endPath   = std::filesystem::path(run.outputDir) / "end_frame.png";
        compute::write_png(startPath.string(), startPreview);
        compute::write_png(endPath.string(), endPreview);
        runner.addArtifact(run.id, Artifact{"start-frame", startPath.string(), "image"});
        runner.addArtifact(run.id, Artifact{"end-frame",   endPath.string(),   "image"});

        const auto t1 = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double, std::milli>(t1 - t0).count();

        runner.setStatus(run.id, "completed");

        const std::string startArtId = run.id + ":start_frame.png";
        const std::string endArtId   = run.id + ":end_frame.png";
        Json resp = {
            {"runId",                run.id},
            {"status",               "completed"},
            {"startFrameArtifactId", startArtId},
            {"startFrameUrl",        "/api/artifacts/content?artifactId="  + startArtId},
            {"startFrameDownloadUrl","/api/artifacts/download?artifactId=" + startArtId},
            {"endFrameArtifactId",   endArtId},
            {"endFrameUrl",          "/api/artifacts/content?artifactId="  + endArtId},
            {"endFrameDownloadUrl",  "/api/artifacts/download?artifactId=" + endArtId},
            {"frameCount",           frameCount},
            {"fps",                  fps},
            {"durationSec",          durSec},
            {"secondsPerOctave",     secondsPerOctave},
            {"depthOctaves",         depth},
            {"targetScale",          2.0 * std::exp(kTop_end)},
            {"width",                previewW},
            {"height",               previewH},
            {"outputWidth",          W},
            {"outputHeight",         H},
            {"generatedMs",          elapsed},
        };
        return resp.dump();
    } catch (const std::exception&) {
        runner.setStatus(run.id, "failed");
        throw;
    }
}

// ─── Unified export: ln-map + final frame + video in one request ─────────────
//
// POST /api/video/export
// {
//   "centerRe", "centerIm", "julia", "juliaRe", "juliaIm",
//   "variant", "colorMap", "iterations",
//   "widthS", "depthOctaves",      // ln-map strip size
//   "fps", "secondsPerOctave", "width", "height"  // video output
// }
//
// Returns {videoArtifactId, lnMapArtifactId, finalFrameArtifactId, ...}.
// When julia=true, the ln-map samples z₀ around center with c=juliaRe+juliaIm·i.

std::string videoExportRoute(const std::filesystem::path& repoRoot, JobRunner& runner, const std::string& body) {
    (void)repoRoot;
    const Json j = parseJsonBody(body);

    const double cr         = j.value("centerRe", 0.0);
    const double ci         = j.value("centerIm", 0.0);
    const bool   julia      = j.value("julia",    false);
    const double jre        = j.value("juliaRe",  0.0);
    const double jim        = j.value("juliaIm",  0.0);
    const std::string variantStr  = j.value("variant",  std::string("mandelbrot"));
    const std::string colormapStr = j.value("colorMap", std::string("classic_cos"));
    const int    iters      = j.value("iterations", 2048);
    const int    fps        = j.value("fps",       30);
    const int    W          = j.value("width",     720);
    const int    H          = j.value("height",    720);
    const bool   localExport = j.value("localExport", false);
    if (fps < 1 || fps > 120) throw std::runtime_error("invalid fps (1..120)");
    validateVideoOutputSize(W, H);
    const double kTop_start = kTopStartForFrame(W, H);
    const double depth      = resolveDepthOctaves(j, kTop_start, 20.0);
    const double secondsPerOctave = resolveSecondsPerOctave(j, depth);
    double durSec = 0.0;
    const int frameCount = frameCountFromSpeed(depth, secondsPerOctave, fps, durSec);
    std::string lnMapColorMode = j.value("lnMapColorMode", std::string("escape"));
    if (!j.contains("lnMapColorMode") && j.contains("colorMode") && !j["colorMode"].is_null()) {
        lnMapColorMode = j.value("colorMode", lnMapColorMode);
    }
    if (!compute::ln_map_color_mode_supported(lnMapColorMode)) {
        throw std::runtime_error("invalid lnMapColorMode (escape|hist_eq|row_eq|log_lift|bands|frontier)");
    }
    const StripPlan stripPlan = resolveStripPlan(j, W, H, depth, lnMapColorMode);
    const int s = stripPlan.actualWidthS;
    const std::string lnMapMode = j.value("lnMapMode", std::string("standard"));
    const std::string lnMapScalar = j.value("lnMapScalar", std::string("auto"));
    const double lnMapFastFp32Depth = j.value("lnMapFastFp32DepthOctaves", 18.0);
    const double lnMapFastFp64Depth = j.value("lnMapFastFp64DepthOctaves", 34.0);
    const bool lnMapFastValidate = j.value("lnMapFastValidate", true);
    const double lnMapFastValidationBandOctaves = j.value("lnMapFastValidationBandOctaves", 4.0);
    const int lnMapFastValidationSampleRows = j.value("lnMapFastValidationSampleRows", 5);
    const int lnMapFastValidationSampleCols = j.value("lnMapFastValidationSampleCols", 24);
    const double lnMapFastValidationMaxMismatchRatio = j.value("lnMapFastValidationMaxMismatchRatio", 0.01);
    const int lnMapFastValidationMaxP99IterDelta = j.value("lnMapFastValidationMaxP99IterDelta", 16);
    const double lnMapFastValidationMaxMeanColorDelta = j.value("lnMapFastValidationMaxMeanColorDelta", 8.0);
    const double lnMapCyclesPerOctave = j.value("lnMapCyclesPerOctave", 0.5);

    if (!(lnMapCyclesPerOctave > 0.0) || lnMapCyclesPerOctave > 64.0 || !std::isfinite(lnMapCyclesPerOctave)) {
        throw std::runtime_error("invalid lnMapCyclesPerOctave (0..64)");
    }
    if (s < 128 || s > 65536)               throw std::runtime_error("invalid widthS (128..65536)");
    if (depth < 0.05 || depth > 120.0)      throw std::runtime_error("invalid depthOctaves (0.05..120)");
    if (iters < 1 || iters > 10000000)      throw std::runtime_error("invalid iterations");
    if (lnMapMode != "standard" && lnMapMode != "fast") {
        throw std::runtime_error("invalid lnMapMode (standard|fast)");
    }
    if (!(lnMapFastFp32Depth >= 0.0) || !(lnMapFastFp64Depth >= 0.0) ||
        !std::isfinite(lnMapFastFp32Depth) || !std::isfinite(lnMapFastFp64Depth)) {
        throw std::runtime_error("invalid ln-map fast depth thresholds");
    }
    if (!(lnMapFastValidationBandOctaves > 0.0) || !std::isfinite(lnMapFastValidationBandOctaves) ||
        lnMapFastValidationSampleRows < 1 || lnMapFastValidationSampleRows > 32 ||
        lnMapFastValidationSampleCols < 1 || lnMapFastValidationSampleCols > 256 ||
        !(lnMapFastValidationMaxMismatchRatio >= 0.0) || lnMapFastValidationMaxMismatchRatio > 1.0 ||
        lnMapFastValidationMaxP99IterDelta < 0 ||
        !(lnMapFastValidationMaxMeanColorDelta >= 0.0) || !std::isfinite(lnMapFastValidationMaxMeanColorDelta)) {
        throw std::runtime_error("invalid ln-map fast validation settings");
    }

    compute::Variant v;
    if (!compute::variant_from_name(variantStr.c_str(), v)) v = compute::Variant::Mandelbrot;
    double bailout = j.contains("bailout") && !j["bailout"].is_null()
        ? j.value("bailout", 2.0)
        : compute::variant_default_bailout(v);
    const double bailoutSq = bailoutSqFromJson(j, bailout, compute::variant_default_bailout_sq(v));
    if (j.contains("bailoutSq") && !j["bailoutSq"].is_null() &&
        !(j.contains("bailout") && !j["bailout"].is_null())) {
        bailout = std::sqrt(bailoutSq);
    }
    if (!(bailout > 0.0) || !std::isfinite(bailout)) throw std::runtime_error("invalid bailout");
    if (!(bailoutSq > 0.0) || !std::isfinite(bailoutSq)) throw std::runtime_error("invalid bailoutSq");
    compute::Colormap cm;
    if (!compute::colormap_from_name(colormapStr.c_str(), cm)) cm = compute::Colormap::ClassicCos;

    auto run = runner.createRun("video-export", body);
    ResourceManager::Lease videoLeaseRaw;
    std::string conflictLock, activeRunId;
    if (!resourceManager().tryAcquire(run.id, "video_export", {"video_export", "cuda_heavy", "cpu_heavy"}, videoLeaseRaw, conflictLock, activeRunId)) {
        runner.setStatus(run.id, "failed");
        throw HttpError(409, Json{
            {"error", "video_export already running"},
            {"activeRunId", activeRunId},
            {"taskType", "video_export"},
            {"resourceLock", conflictLock},
        }.dump());
    }
    auto videoLease = std::make_shared<ResourceManager::Lease>(std::move(videoLeaseRaw));
    runner.setCancelable(run.id, true);

    auto execute = [=, &runner]() mutable -> Json {
    (void)videoLease;
    runner.setStatus(run.id, "running");
    try {
        const auto t0 = std::chrono::steady_clock::now();
        throwIfCancelled(runner, run.id);

        // ── 1. Render final cartesian frame ────────────────────────────────────
        setVideoProgress(runner, run.id, "final_frame", 0, 1, depth, depth);
        const double kTop_end   = kTop_start - depth * LN_TWO;

        compute::MapParams mp;
        mp.center_re  = cr;
        mp.center_im  = ci;
        mp.scale      = 2.0 * std::exp(kTop_end);
        mp.width      = W;
        mp.height     = H;
        mp.iterations = iters;
        mp.bailout    = bailout;
        mp.bailout_sq = bailoutSq;
        mp.variant    = v;
        mp.metric     = compute::Metric::Escape;
        mp.colormap   = cm;
        mp.smooth     = false;
        mp.julia      = julia;
        mp.julia_re   = jre;
        mp.julia_im   = jim;
        mp.engine     = "auto";
        mp.scalar_type = "auto";

        cv::Mat finalImg(H, W, CV_8UC3);
        compute::MapStats finalStats;
        compute::FieldOutput finalField;   // held for deferred coloring (non-escape modes)
        bool finalFrameDeferred = false;
        if (lnMapColorMode == "escape") {
            finalStats = compute::render_map(mp, finalImg);
        } else {
            // Compute the iteration field now, but defer coloring until after the strip:
            // hist_eq reuses the strip's shared equalization for a seamless warp blend.
            finalStats = compute::render_map_field(mp, finalField);
            finalStats.engine_used += "_ln_" + lnMapColorMode;
            finalFrameDeferred = true;
        }
        throwIfCancelled(runner, run.id);

        const std::filesystem::path finalPath = std::filesystem::path(run.outputDir) / "final_frame.png";
        if (!finalFrameDeferred) {
            compute::write_png(finalPath.string(), finalImg);
            runner.addArtifact(run.id, Artifact{"final-frame", finalPath.string(), "image"});
        }
        setVideoProgress(runner, run.id, "final_frame", 1, 1, depth, depth,
                         "", "", Json{{"engine", finalStats.engine_used}, {"scalar", finalStats.scalar_used}, {"finalFrameEngine", finalStats.engine_used}, {"finalFrameScalar", finalStats.scalar_used}});

        // ── 2. Render ln-map strip ─────────────────────────────────────────────
        const int t = stripPlan.heightT;
        cv::Mat strip(t, s, CV_8UC3);
        compute::LnMapParams lp;
        lp.julia = julia;
        lp.center_re = cr;
        lp.center_im = ci;
        lp.julia_re = jre;
        lp.julia_im = jim;
        lp.width_s = s;
        lp.height_t = t;
        lp.iterations = iters;
        lp.bailout = bailout;
        lp.bailout_sq = bailoutSq;
        lp.variant = v;
        lp.colormap = cm;
        lp.color_mode = lnMapColorMode;
        lp.color_cycles_per_octave = lnMapCyclesPerOctave;
        lp.engine = j.value("lnMapEngine", std::string("auto"));
        lp.precision_mode = lnMapMode;
        lp.scalar_type = lnMapScalar;
        lp.fast_fp32_depth_octaves = lnMapFastFp32Depth;
        lp.fast_fp64_depth_octaves = lnMapFastFp64Depth;
        lp.fast_validate = lnMapFastValidate;
        lp.fast_validation_band_octaves = lnMapFastValidationBandOctaves;
        lp.fast_validation_sample_rows = lnMapFastValidationSampleRows;
        lp.fast_validation_sample_cols = lnMapFastValidationSampleCols;
        lp.fast_validation_max_mismatch_ratio = lnMapFastValidationMaxMismatchRatio;
        lp.fast_validation_max_p99_iter_delta = lnMapFastValidationMaxP99IterDelta;
        lp.fast_validation_max_mean_color_delta = lnMapFastValidationMaxMeanColorDelta;
        setVideoProgress(runner, run.id, "ln_map", 0, t, 0.0, depth,
                         "", "", Json{{"lnMapColorMode", lnMapColorMode}, {"currentLnMapRow", 0}, {"totalLnMapRows", t}});
        throwIfCancelled(runner, run.id);
        const compute::LnMapStats lnStats = compute::render_ln_map(
            lp, strip,
            [&](int rowsDone) {
                throwIfCancelled(runner, run.id);
                const double octave = depth * static_cast<double>(rowsDone) / std::max(1, t);
                setVideoProgress(runner, run.id, "ln_map", rowsDone, t, octave, depth,
                                 "", "", Json{{"lnMapColorMode", lnMapColorMode}, {"currentLnMapRow", rowsDone}, {"totalLnMapRows", t}});
            });
        throwIfCancelled(runner, run.id);
        setVideoProgress(runner, run.id, "ln_map", t, t, depth, depth,
                         "", "", Json{{"engine", lnStats.engine_used}, {"scalar", lnStats.scalar_used}, {"lnMapEngine", lnStats.engine_used}, {"lnMapScalar", lnStats.scalar_used}, {"lnMapMode", lnStats.precision_mode}, {"lnMapColorMode", lnMapColorMode}, {"lnMapLayerSummary", lnStats.layer_summary}, {"lnMapValidationSummary", lnStats.validation_summary}, {"currentLnMapRow", t}, {"totalLnMapRows", t}});

        const std::filesystem::path stripPath = std::filesystem::path(run.outputDir) / "ln_map.png";
        compute::write_png(stripPath.string(), strip);
        runner.addArtifact(run.id, Artifact{"ln-map", stripPath.string(), "image"});

        // Colorize the deferred final frame now that the strip's shared equalization
        // is available (hist_eq), so the strip↔final-frame warp blend is seamless.
        if (finalFrameDeferred) {
            throwIfCancelled(runner, run.id);
            colorizeFinalFrameWithLnMapMode(mp, finalField, lnMapColorMode, lnStats.equalization, finalImg);
            finalField = compute::FieldOutput{};  // release the field buffer before warp/encode
            compute::write_png(finalPath.string(), finalImg);
            runner.addArtifact(run.id, Artifact{"final-frame", finalPath.string(), "image"});
        }

        // Sidecar so old zoomVideoRoute can also consume this ln-map.
        {
            Json sc = {
                {"centerRe", cr}, {"centerIm", ci},
                {"julia", julia}, {"juliaRe", jre}, {"juliaIm", jim},
                {"widthS", s}, {"actualWidthS", s}, {"fullWidthS", stripPlan.fullWidthS},
                {"heightT", t}, {"depthOctaves", depth},
                {"lnMapExtraOctaves", stripPlan.extraOctaves},
                {"qualityPreset", stripPlan.qualityPreset},
                {"qualityScale", stripPlan.qualityScale},
                {"estimatedPeakMemory", stripPlan.estimatedPeakMemory},
                {"lnRadiusTop", LN_FOUR}, {"variant", variantStr},
                {"colorMap", colormapStr}, {"iterations", iters},
                {"lnMapColorMode", lnMapColorMode},
                {"lnMapCyclesPerOctave", lnMapCyclesPerOctave},
                {"bailout", bailout},
                {"bailoutSq", bailoutSq},
                {"engine", lnStats.engine_used},
                {"scalar", lnStats.scalar_used},
                {"precisionMode", lnStats.precision_mode},
                {"layerSummary", lnStats.layer_summary},
                {"validationSummary", lnStats.validation_summary},
            };
            const std::filesystem::path scPath = std::filesystem::path(run.outputDir) / "ln_map.json";
            atomicWriteText(scPath, sc.dump(2));
        }

        // ── 3. Render first/last preview frames ───────────────────────────────
        throwIfCancelled(runner, run.id);
        const bool preferCudaWarp = j.value("cudaWarp", true);
        const cv::Mat startPreview = renderZoomPreviewFrameSmart(strip, finalImg, W, H, kTop_start, kTop_end, preferCudaWarp);
        const cv::Mat endPreview   = renderZoomPreviewFrameSmart(strip, finalImg, W, H, kTop_end,   kTop_end, preferCudaWarp);
        const std::filesystem::path startPreviewPath = std::filesystem::path(run.outputDir) / "start_frame.png";
        const std::filesystem::path endPreviewPath   = std::filesystem::path(run.outputDir) / "end_frame.png";
        compute::write_png(startPreviewPath.string(), startPreview);
        compute::write_png(endPreviewPath.string(), endPreview);
        runner.addArtifact(run.id, Artifact{"start-frame", startPreviewPath.string(), "image"});
        runner.addArtifact(run.id, Artifact{"end-frame",   endPreviewPath.string(),   "image"});

        // ── 4. Generate video ──────────────────────────────────────────────────
        setVideoProgress(runner, run.id, "video_warp_encode", 0, frameCount, depth, depth,
                         "", "", Json{{"currentFrame", 0}, {"totalFrames", frameCount}, {"lnMapEngine", lnStats.engine_used}, {"lnMapScalar", lnStats.scalar_used}, {"lnMapMode", lnStats.precision_mode}, {"lnMapColorMode", lnMapColorMode}, {"lnMapLayerSummary", lnStats.layer_summary}, {"lnMapValidationSummary", lnStats.validation_summary}, {"finalFrameEngine", finalStats.engine_used}, {"finalFrameScalar", finalStats.scalar_used}});
        std::string ffmpegStderr;
        std::string encoderUsed;
        std::string warpMethod;
        VideoWarpStats warpStats;
        const std::string videoPath = generateZoomVideo(
            strip, finalImg, W, H, fps, frameCount,
            kTop_start, kTop_end, depth,
            std::filesystem::path(run.outputDir), "zoom",
            [&](int frameDone) {
                setVideoProgress(runner, run.id, "video_warp_encode", frameDone, frameCount, depth, depth,
                                 "", "", Json{{"currentFrame", frameDone}, {"totalFrames", frameCount}, {"lnMapEngine", lnStats.engine_used}, {"lnMapScalar", lnStats.scalar_used}, {"lnMapMode", lnStats.precision_mode}, {"lnMapColorMode", lnMapColorMode}, {"lnMapLayerSummary", lnStats.layer_summary}, {"lnMapValidationSummary", lnStats.validation_summary}, {"finalFrameEngine", finalStats.engine_used}, {"finalFrameScalar", finalStats.scalar_used}});
            },
            &ffmpegStderr,
            &encoderUsed,
            preferCudaWarp,
            &warpMethod,
            &warpStats,
            [&]() { return runner.isCancelRequested(run.id); }
        );
        throwIfCancelled(runner, run.id);
        Json doneDetails = {{"currentFrame", frameCount}, {"totalFrames", frameCount}, {"lnMapEngine", lnStats.engine_used}, {"lnMapScalar", lnStats.scalar_used}, {"lnMapMode", lnStats.precision_mode}, {"lnMapColorMode", lnMapColorMode}, {"lnMapLayerSummary", lnStats.layer_summary}, {"lnMapValidationSummary", lnStats.validation_summary}, {"finalFrameEngine", finalStats.engine_used}, {"finalFrameScalar", finalStats.scalar_used}, {"warpMethod", warpMethod}, {"encoder", encoderUsed}};
        mergeVideoWarpStats(doneDetails, warpStats);
        setVideoProgress(runner, run.id, "video_warp_encode", frameCount, frameCount, depth, depth,
                         "", "", doneDetails);
        const std::string videoFile = std::filesystem::path(videoPath).filename().string();
        runner.addArtifact(run.id, Artifact{"zoom-video", videoPath, "video"});

        Json renderLog = {
            {"finalFrameEngine", finalStats.engine_used},
            {"finalFrameScalar", finalStats.scalar_used},
            {"lnMapEngine", lnStats.engine_used},
            {"lnMapScalar", lnStats.scalar_used},
            {"lnMapMode", lnStats.precision_mode},
            {"lnMapColorMode", lnMapColorMode},
            {"lnMapLayerSummary", lnStats.layer_summary},
            {"lnMapValidationSummary", lnStats.validation_summary},
            {"warpMethod", warpMethod},
            {"encoder", encoderUsed},
            {"durationSec", durSec},
            {"fps", fps},
            {"frameCount", frameCount},
            {"qualityPreset", stripPlan.qualityPreset},
            {"qualityScale", stripPlan.qualityScale},
            {"estimatedPeakMemory", stripPlan.estimatedPeakMemory},
            {"lnMapExtraOctaves", stripPlan.extraOctaves},
        };
        mergeVideoWarpStats(renderLog, warpStats);
        const std::filesystem::path reportPath = std::filesystem::path(run.outputDir) / "video_export.json";
        atomicWriteText(reportPath, renderLog.dump(2));
        runner.addArtifact(run.id, Artifact{"video-export", reportPath.string(), "report"});

        const auto t1 = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double, std::milli>(t1 - t0).count();

        runner.setStatus(run.id, "completed");

        const std::string lnArtId    = run.id + ":ln_map.png";
        const std::string finalArtId = run.id + ":final_frame.png";
        const std::string startArtId = run.id + ":start_frame.png";
        const std::string endArtId   = run.id + ":end_frame.png";
        const std::string videoArtId = run.id + ":" + videoFile;
        const std::string reportArtId = run.id + ":video_export.json";

        Json resp = {
            {"runId",              run.id},
            {"status",             "completed"},
            {"videoArtifactId",    videoArtId},
            {"videoUrl",           "/api/artifacts/content?artifactId="  + videoArtId},
            {"videoDownloadUrl",   "/api/artifacts/download?artifactId=" + videoArtId},
            {"videoLocalPath",     videoPath},
            {"lnMapArtifactId",    lnArtId},
            {"lnMapDownloadUrl",   "/api/artifacts/download?artifactId=" + lnArtId},
            {"lnMapLocalPath",     stripPath.string()},
            {"finalFrameArtifactId", finalArtId},
            {"finalFrameDownloadUrl", "/api/artifacts/download?artifactId=" + finalArtId},
            {"finalFrameLocalPath", finalPath.string()},
            {"startFrameArtifactId", startArtId},
            {"startFrameUrl",      "/api/artifacts/content?artifactId="  + startArtId},
            {"startFrameDownloadUrl", "/api/artifacts/download?artifactId=" + startArtId},
            {"startFrameLocalPath", startPreviewPath.string()},
            {"endFrameArtifactId", endArtId},
            {"endFrameUrl",        "/api/artifacts/content?artifactId="  + endArtId},
            {"endFrameDownloadUrl", "/api/artifacts/download?artifactId=" + endArtId},
            {"endFrameLocalPath",  endPreviewPath.string()},
            {"reportArtifactId",   reportArtId},
            {"reportDownloadUrl",  "/api/artifacts/download?artifactId=" + reportArtId},
            {"reportLocalPath",    reportPath.string()},
            {"localExport",        localExport},
            {"frameCount",         frameCount},
            {"fps",                fps},
            {"durationSec",        durSec},
            {"secondsPerOctave",   secondsPerOctave},
            {"depthOctaves",       depth},
            {"targetScale",        2.0 * std::exp(kTop_end)},
            {"fullWidthS",         stripPlan.fullWidthS},
            {"actualWidthS",       s},
            {"heightT",            t},
            {"lnMapExtraOctaves",  stripPlan.extraOctaves},
            {"qualityPreset",      stripPlan.qualityPreset},
            {"qualityScale",       stripPlan.qualityScale},
            {"estimatedPeakMemory", stripPlan.estimatedPeakMemory},
            {"width",              W},
            {"height",             H},
            {"generatedMs",        elapsed},
            {"finalFrameEngine",   finalStats.engine_used},
            {"finalFrameScalar",   finalStats.scalar_used},
            {"lnMapEngine",        lnStats.engine_used},
            {"lnMapScalar",        lnStats.scalar_used},
            {"lnMapMode",          lnStats.precision_mode},
            {"lnMapColorMode",     lnMapColorMode},
            {"lnMapCyclesPerOctave", lnMapCyclesPerOctave},
            {"lnMapLayerSummary",  lnStats.layer_summary},
            {"lnMapValidationSummary", lnStats.validation_summary},
            {"warpMethod",         warpMethod},
            {"encoder",            encoderUsed},
            {"ffmpegStderr",       ffmpegStderr},
        };
        mergeVideoWarpStats(resp, warpStats);
        return resp;

    } catch (const std::exception& e) {
        if (runner.isCancelRequested(run.id) || std::string(e.what()) == "cancelled") {
            setVideoProgress(runner, run.id, "cancelled", 0, frameCount, 0.0, depth, "video_export", "cancelled");
            runner.setStatus(run.id, "cancelled");
        } else {
            setVideoProgress(runner, run.id, "failed", 0, 0, 0.0, depth, "video_export", e.what());
            runner.setStatus(run.id, "failed");
        }
        throw;
    }
    };

    const bool background = j.value("background", true);
    if (background) {
        setVideoProgress(runner, run.id, "queued", 0, frameCount, 0.0, depth);
        std::thread([execute]() mutable {
            try {
                (void)execute();
            } catch (...) {}
        }).detach();

        const bool queuedRemapSafe = (s + 1) < OPENCV_REMAP_DIM_LIMIT
            && stripPlan.heightT < OPENCV_REMAP_DIM_LIMIT
            && W < OPENCV_REMAP_DIM_LIMIT
            && H < OPENCV_REMAP_DIM_LIMIT;
        std::string queuedWarpMethod = queuedRemapSafe ? "opencv_cpu_remap_precomputed" : "manual_cpu_bilinear_precomputed";
#if USE_CUDA_VIDEO_WARP
        if (j.value("cudaWarp", true) && fsd_cuda::cuda_video_warp_available()) {
            queuedWarpMethod = "cuda_texture";
        }
#endif

        Json resp = {
            {"runId", run.id},
            {"status", "queued"},
            {"localExport", localExport},
            {"frameCount", frameCount},
            {"fps", fps},
            {"durationSec", durSec},
            {"secondsPerOctave", secondsPerOctave},
            {"depthOctaves", depth},
            {"fullWidthS", stripPlan.fullWidthS},
            {"actualWidthS", s},
            {"heightT", stripPlan.heightT},
            {"lnMapExtraOctaves", stripPlan.extraOctaves},
            {"qualityPreset", stripPlan.qualityPreset},
            {"qualityScale", stripPlan.qualityScale},
            {"estimatedPeakMemory", stripPlan.estimatedPeakMemory},
            {"width", W},
            {"height", H},
            {"lnMapEngine", "openmp"},
            {"lnMapScalar", "fp64"},
            {"lnMapMode", lnMapMode},
            {"lnMapColorMode", lnMapColorMode},
            {"lnMapCyclesPerOctave", lnMapCyclesPerOctave},
            {"lnMapLayerSummary", ""},
            {"lnMapValidationSummary", ""},
            {"warpMethod", queuedWarpMethod},
        };
        return resp.dump();
    }

    return execute().dump();
}

} // namespace fsd
