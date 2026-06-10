# Video Pipeline / 视频导出链路

这份文档说明 ln-map、preview、统一视频导出、旧 zoom-video 路径、warp/encode 和进度 artifact。

## Entry Points / 入口

| Endpoint | Purpose |
|---|---|
| `POST /api/map/ln` | 单独渲染 ln-map strip PNG 和 sidecar JSON。 |
| `POST /api/video/preview` | 只渲染起止预览帧，不生成 ln-map 和 MP4。 |
| `POST /api/video/export` | 统一导出：final frame + ln-map + preview frames + MP4 + report。 |
| `POST /api/video/zoom` | 旧路径：从已有 ln-map artifact 生成 MP4。 |

主要代码：

- `backend/src/api/routes_ln.cpp`
- `backend/src/api/routes_video.cpp`
- `backend/src/compute/ln_map.hpp`
- `backend/src/compute/ln_map.cpp`
- `backend/src/compute/ln_map_avx2.cpp`
- `backend/src/compute/ln_map_avx512.cpp`
- `backend/src/compute/cuda/ln_map.cu`
- `backend/src/compute/cuda/video_warp.cu`
- `backend/src/compute/map_kernel.cpp`
- `frontend/src/views/MapView.vue`

## ln-map Geometry / ln-map 几何

ln-map strip 的列是 angle，行是 log radius：

```text
theta = 2*pi*x/s
k = ln(4) - row * 2*pi/s
c = center + exp(k) * (cos(theta) + i*sin(theta))
```

一次渲染 strip 后，视频帧通过从 strip 取样生成，不需要每帧重新算 fractal。

Strip height:

```text
t = ceil((2 + depthOctaves) * ln(2) / (2*pi) * widthS)
```

`2` 个额外 octave 是为了覆盖视频首帧边缘和采样安全区。

Ln-map coloring:

- `lnMapColorMode="escape"` 保持原来的逐像素 escape-time 映射。
- `lnMapColorMode="hist_eq"` 先统计整张 strip 中 `radius <= 2` 且已逃逸像素的迭代次数直方图，再用 CDF 做类似直方图均衡化的映射。最终颜色仍使用 `colorMap` 指定的调色板，并按 ln-map 深度轻微调整 palette window/phase，让深层区域保留更多色彩分离。

## Preview Flow / 预览流程

`/api/video/preview` 不建 ln-map。它直接渲染两个 cartesian frame：

```text
kTop_start = ln(4) - ln(sqrt(aspect^2 + 1))
kTop_end = kTop_start - depthOctaves * ln(2)
scale = 2 * exp(kTop)
```

这条路径用于 UI 快速确认 zoom 终点和时长，成本远低于完整视频。

## Unified Export Flow / 统一导出流程

`/api/video/export` 默认后台执行：

```text
createRun("video-export")
  -> acquire video_export/cuda_heavy/cpu_heavy locks
  -> render final_frame.png at kTop_end
  -> render ln_map.png + ln_map.json
  -> render start_frame.png and end_frame.png
  -> generate zoom.mp4 by warping strip + final frame
  -> write video_export.json report
  -> register artifacts
```

主要 artifacts：

- `final_frame.png`
- `ln_map.png`
- `ln_map.json`
- `start_frame.png`
- `end_frame.png`
- `zoom.mp4`
- `video_export.json`

## Warp And Encode / warp 与编码

每一帧由 `generateZoomVideo()` 生成。采样规则：

```text
if strip_row is inside strip:
    sample ln_map strip
else:
    sample final cartesian frame
```

warp 方法优先级：

1. CUDA video warp，如果编译和运行时可用且 `cudaWarp=true`
2. OpenCV remap，前提是 strip/frame 尺寸低于 OpenCV remap 的 short 坐标限制
3. manual CPU bilinear fallback

编码优先级：

1. `ffmpeg` + `h264_nvenc`
2. `ffmpeg` + `hevc_nvenc`
3. `ffmpeg` + `libx264`
4. OpenCV `VideoWriter` mp4v
5. OpenCV `VideoWriter` MJPG AVI fallback

响应和 report 会记录 `warpMethod`、`encoder`、`ffmpegStderr`、平均 warp/write 时间等统计。

## Quality Presets / 质量参数

`widthS` 可以直接指定。未指定时根据输出尺寸推导最小 strip width：

```text
fullWidthS = ceil(sqrt(W^2 + H^2) * pi)
```

再由 preset 决定实际宽度：

| Preset | Scale |
|---|---|
| `draft` | `0.35` |
| `balanced` | `0.55` |
| `high` | `0.75` |
| `full` | `1.0` |

4K 及以上默认 `balanced`，其它默认 `high`。

## ln-map Precision Modes / ln-map 精度模式

`lnMapMode`:

- `standard`: 单一路径渲染。
- `fast`: 按深度分层使用更快 scalar，并可做 validation。

相关参数：

- `lnMapScalar`
- `lnMapFastFp32DepthOctaves`
- `lnMapFastFp64DepthOctaves`
- `lnMapFastValidate`
- `lnMapFastValidationBandOctaves`
- `lnMapFastValidationSampleRows`
- `lnMapFastValidationSampleCols`
- `lnMapFastValidationMaxMismatchRatio`
- `lnMapFastValidationMaxP99IterDelta`
- `lnMapFastValidationMaxMeanColorDelta`

精度实验记录见 [lnmap_precision_experiments.md](lnmap_precision_experiments.md)。

## Progress And Cancellation / 进度与取消

`video-export` 使用 `JobRunner` progress stages：

- `queued`
- `final_frame`
- `ln_map`
- `video_warp_encode`
- `completed`
- `cancelled`
- `failed`

统一导出支持 cancel。route 会轮询 `runner.isCancelRequested(run.id)` 并在 ln-map 行进度、warp encode frame 进度中更新 ETA。

## Operational Notes / 运维注意

- 需要 MP4 导出时应安装 `ffmpeg`。
- `video_export` 会申请 `video_export`、`cuda_heavy`、`cpu_heavy` locks，同一时间只能跑一个重视频任务。
- 大尺寸视频会占用大量内存；响应里的 `estimatedPeakMemory` 可用于 UI 告警。
- 手机/平板访问局域网 dev server 时，确保 backend `18080` 对设备可达。
