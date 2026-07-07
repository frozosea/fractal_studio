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

Video export no longer has to allocate this whole strip as one image. If the
logical `heightT` is larger than `lnMapMaxSegmentHeight` (default `8192` rows),
`/api/video/export` splits the zoom into multiple ln-map segments. Each segment
has:

- a bounded-height strip (`ln_map.png`, `ln_map_001.png`, ...)
- its own local final frame for the center fallback (`final_frame_000.png`, ...)
- a row offset recorded in `ln_map.json`

The chunk videos are encoded independently and concatenated into the usual
`zoom.mp4`. Responses and reports include `lnMapSegmented`,
`lnMapSegmentCount`, `lnMapMaxSegmentHeight`, `lnMapTotalSegmentRows`,
`estimatedPeakMemory` (bounded segment peak), and
`estimatedSingleStripMemory` (what the old one-piece strip would have needed).
For `lnMapColorMode="hist_eq"`, segmented export first streams the logical
one-piece strip in bounded chunks to build one global equalization/periodic
coloring table, then renders every segment with that shared table. If the
request supplies a compatible `lnMapStatsRunId` from a small preview run,
`hist_eq` can reuse that preview equalization instead, which makes the final
video match the tuned preview more closely. This avoids per-segment direct-color
discontinuities without allocating the full strip.
The legacy `/api/video/zoom` route intentionally rejects segmented ln-map
artifacts because it only knows how to warp a single strip.

Ln-map coloring:

`colorMap` 选择实际调色板；`lnMapColorMode` 选择 escape iteration 到调色板坐标的映射方式。

- `lnMapColorMode="escape"` 保持原来的逐像素 escape-time 映射。
- `lnMapColorMode="hist_eq"` 做**周期性离散上色**，直接以逃逸次数为浮点相位（不做直方图均衡、不做平滑）：`phase = (count − count_min) / period`。`period` 由整张 strip 的逃逸次数分布定：取**中位数**逃逸次数 `median`（避免被单个深层 minibrot 拉偏导致整体偏绿），令 `count_max = count_min + 2·(median − count_min)`，`period = (count_max − count_min) / (total_octaves · lnMapCyclesPerOctave)`，`total_octaves = heightT·2π/(widthS·ln2) ≈ log2(放大倍率)`。每个逃逸次数 = 一条实色带。循环型调色板（`classic_cos`/`hsv_wheel`/`tri765`/`twilight`/`spectral1530`）用 `frac`，非循环型用三角波反射避免接缝。开场（相位最低、即 zoom 起始的最浅层）的前 1/6 周期从 `(0,0,0)` 渐变到调色板起点色（`spectral1530` 为绿 `(0,255,0)`），随后接 1530 色环。`lnMapCyclesPerOctave` 默认 `1.0`（每倍频 1 个周期，即「放大倍率的对数为周期数」），密度可调。
  逃逸次数 field 按几何（center/depth/widthS/iterations/variant…）缓存，重新上色（改 `colorMap`/`lnMapCyclesPerOctave`）会复用已算的 field，只走一遍上色（≈50ms），不重算迭代——适合导出视频前调色。`spectral1530` 在 escape 模式也用同一调色板：iter<255 走黑→绿，之后走 1530 色环。最终 cartesian 帧复用 strip 的同一套均衡 LUT，使 strip↔final-frame 的 warp 混合无色彩接缝；分段视频导出也复用同一套全局 LUT，避免段边界跳色。`bands`/`frontier` 在分段导出时同样复用整条 strip 的共享 CDF。
- `lnMapColorMode="row_eq"` 对每个 ln-radius 行单独做 escape iteration 秩映射。它强化每个深度切片内部的角向细节，代价是弱化全局 escape-time 尺度。
- `lnMapColorMode="log_lift"` 对归一化 escape iteration 做 `log1p` 拉伸，不依赖直方图。低迭代差异会更明显，高迭代区域会被压缩，适合柔和预览或避免统计闪动。
- `lnMapColorMode="bands"` 混合全局 CDF 与粗/细周期色带，把逃逸时间变化转成更可读的等值轮廓。
- `lnMapColorMode="frontier"` 以全局 CDF 为基础，再根据邻域 escape-rank 梯度提亮边界，适合突出细丝、小轮廓和分界脉络。

除 `escape` 外，这些映射都需要先计算 ln-map iteration field；分段视频导出会把 field 限制在当前统计或渲染 chunk 内。

## Preview Flow / 预览流程

`/api/video/preview` 现在会建一条小尺寸 ln-map preview strip，再用和完整导出
相同的 warp/composite 逻辑生成两个 frame：

```text
kTop_start = ln(4) - ln(sqrt(aspect^2 + 1))
kTop_end = kTop_start - depthOctaves * ln(2)
scale = 2 * exp(kTop)
```

这条路径用于 UI 快速确认 zoom 终点、构图和上色。对 `hist_eq`/`bands`/`frontier`
这类整体染色模式，统计来自 preview strip，是低成本近似；完整导出默认仍可重新
统计全尺寸 strip。

## Unified Export Flow / 统一导出流程

`/api/video/export` 默认后台执行：

```text
createRun("video-export")
  -> acquire video_export/cuda_heavy/cpu_heavy locks
  -> render final_frame.png at kTop_end
  -> optionally build global ln-map color statistics (segmented hist_eq/bands/frontier)
  -> render ln_map.png + ln_map.json
  -> render start_frame.png and end_frame.png
  -> generate zoom.mp4 by warping strip + final frame
  -> write video_export.json report
  -> register artifacts
```

The backend binary also exposes local CLI entry points for long-running exports:

```bash
./backend/build/fractal_studio_backend export-map --json '{"taskType":"still_export", ...}'
./backend/build/fractal_studio_backend export-video --json '{"centerRe":-0.75, ...}'
```

`export-video` starts the same background job and prints stage progress while the
run writes normal artifacts under `runtime/runs/videos/<runId>/`.

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
- `ln_map_equalization`（segmented `hist_eq`/`bands`/`frontier` 的全局统计 pass）
- `ln_map_render`（这些全局染色模式的实际条带渲染 pass）
- `video_warp_encode`
- `completed`
- `cancelled`
- `failed`

统一导出支持 cancel。route 会轮询 `runner.isCancelRequested(run.id)` 并在 ln-map 行进度、warp encode frame 进度中更新 ETA。

## Operational Notes / 运维注意

- 需要 MP4 导出时应安装 `ffmpeg`。
- `video_export` 会申请 `video_export`、`cuda_heavy`、`cpu_heavy` locks，同一时间只能跑一个重视频任务。
- 大尺寸视频会占用大量内存；响应里的 `estimatedPeakMemory` 可用于 UI 告警。
- segmented `hist_eq`/`bands`/`frontier` 的全局统计 pass 会优先使用 CUDA 直接生成 escape-count histogram/CDF；不能走 CUDA 时使用多线程 CPU histogram，并跳过整行全在/全不在 `|c|<=2` 的几何检查。
- 视频 warp/compositing 支持 CUDA texture path；分段导出也会把 segment row offset 传给 CUDA kernel。CPU fallback 使用 OpenCV remap + 并行行合成。
- 手机/平板访问局域网 dev server 时，确保 backend `18080` 对设备可达。
