# Video Pipeline / 视频导出链路

这份文档说明 ln-map、preview、统一视频导出、旧 zoom-video 路径、warp/encode 和进度 artifact。

## Entry Points / 入口

| Endpoint | Purpose |
|---|---|
| `POST /api/map/ln` | 单独渲染 ln-map strip PNG 和 sidecar JSON。 |
| `POST /api/video/preview` | 渲染小尺寸 ln-map、起止预览帧和预览统计，不生成 MP4。 |
| `POST /api/video/export` | 统一导出：final frame + ln-map + preview frames + MP4 + report。 |
| `POST /api/video/zoom` | 旧路径：从已有 ln-map artifact 生成 MP4。 |
| `POST /api/video/transition-preview` | 预览双变体 transition 的 rotation 或 zoom 起止帧。 |
| `POST /api/video/transition` | 导出双变体 transition rotation 或 zoom 视频。 |

ln-map 与普通 zoom 视频当前只支持内置 variant。自定义公式仍可用于 2D map，但请求 ln-map 或视频时会明确返回 `400`，不会再静默按 Mandelbrot 生成错误产物。多变体 transition 视频和分段 ln-map 产物复用的暂缓原因与恢复条件见 [feature_status.md](feature_status.md)。

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

Video export normally allocates one strip and creates the movie from
`ln_map.png + final_frame.png`. If the logical `heightT` is larger than
`lnMapMaxSegmentHeight` (default `8192` rows), `/api/video/export` uses segmented
strips as a memory fallback. Segment strips are written to a temporary
directory, then streamed sequentially into one ffmpeg pipe; the normal output
directory does not keep `zoom_part_*.mp4` files or concat lists unless
`keepIntermediateVideoFiles=true`.

分段导出只做**一次** cartesian 渲染：整条 zoom 的真正 final frame。每个
非末段的 fallback frame（该段最深几帧画面中心 strip 数据耗尽处的补充数据）
不再逐段重新渲染，而是在所有 strip 落盘后**从后往前合成**：
`fallback_i = warp(strip_{i+1}, fallback_{i+1})`，链条终点是唯一的 final
frame。合成是纯重采样（每帧 ~几十 ms），分形迭代只发生在 strip 和一张
final frame 上——每个点只算一次。内存仍然有界（同一时刻只持有一条 strip
和两帧）。

Responses and reports include `lnMapSegmented`,
`lnMapSegmentCount`, `lnMapMaxSegmentHeight`, `lnMapTotalSegmentRows`,
`estimatedPeakMemory` (bounded segment peak), and
`estimatedSingleStripMemory` (what the old one-piece strip would have needed).
For `lnMapColorMode="hist_eq"`, segmented export first streams the logical
one-piece strip in bounded chunks to build one global equalization/periodic
coloring table, then renders every segment with that shared table. If the
request supplies a compatible `lnMapStatsRunId` from a small preview run,
`hist_eq` reuses that preview equalization instead (`lnMapStatsReused=true`),
which skips the export statistics pass and makes the final video match the tuned
preview more closely. This avoids per-segment direct-color discontinuities
without allocating the full strip.

“compatible” 会校验完整生成身份：精确 center 字符串采用双向严格匹配、
Julia 模式与参数、variant、iterations、bailout radius/square、color map/mode、
cycles、precision/engine/scalar、fast precision ladder 与验证参数，以及额外 octave；
preview depth 也必须相容。任一项变化时 preview stats 不复用，导出会重新计算统计。
非分段 `lnMapRunId` 复用执行同一身份校验，并继续严格校验 strip width/depth/height；
不匹配会明确失败，不会把旧位置的 strip 接到当前视频。`bands`/`frontier` 的
single-strip 缓存尚未持久化全局 CDF，因此当前明确拒绝这两种模式的 `lnMapRunId`
复用，避免 cached strip 与新 final frame 使用不同 CDF 产生接缝。

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
这类整体染色模式，统计来自 preview strip，是低成本近似。`hist_eq` 完整导出
会优先复用兼容的 preview equalization；复用时不会再跑 `ln_map_equalization`
统计阶段，只会渲染高清导出 strip。

## Unified Export Flow / 统一导出流程

`/api/video/export` 默认后台执行：

```text
createRun("video-export")
  -> acquire video_export/cuda_heavy/cpu_heavy locks
  -> render final_frame.png at kTop_end
  -> optionally build global ln-map color statistics (only when preview stats cannot be reused)
  -> render ln_map.png + ln_map.json
  -> render start_frame.png and end_frame.png
  -> generate zoom.mp4 by warping strip + final frame into one encoder stream
  -> write video_export.json report
  -> register artifacts
```

The backend binary also exposes local CLI entry points for long-running exports:
Run these commands from the project root (`fractal_studio/`):

```bash
./runtime/build/fractal_studio_backend export-map --json '{"taskType":"still_export", ...}'
./runtime/build/fractal_studio_backend export-video --json '{"centerRe":-0.75, ...}'
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

每一帧由 `generateZoomVideo()` / `generateZoomVideoSequence()` 生成。单片和分片
都直接把 raw BGR frame 写进同一个编码器流。采样规则：

```text
if strip_row is inside strip:
    sample ln_map strip
else:
    sample final cartesian frame
```

分段导出时，非末段的 fallback frame 位于 `endDepth + extraOctaves −
log2(sqrt(aspect²+1)) − 0.15` oct 处（而不是 strip 底部深度）：帧像素能延伸到
屏幕对角（corner 半径 = rMax × 半高），fallback frame 的**半高**必须盖住
strip 数据耗尽的圆盘，否则每段最深的几帧会在画面中心出现「黑圈套矩形」
（圈 = strip 底部圆盘，矩形 = fallback frame 的覆盖范围）。`resolveStripPlan`
会保证 `extraOctaves` 至少留出这个余量；这个余量同时保证 back-to-front
合成时每个采样都落在下一段 strip + fallback 的覆盖范围内。

warp 方法优先级：

1. CUDA video warp，如果编译和运行时可用且 `cudaWarp=true`
2. OpenCV remap，前提是 strip/frame 尺寸低于 OpenCV remap 的 short 坐标限制
3. manual CPU bilinear fallback

编码优先级（每个硬件编码器先用 1 帧黑帧**实测探测**——ffmpeg 即使在无
对应 GPU 的机器上也带着 nvenc/vaapi 编码器，光查列表会误判；探测结果按
进程缓存）：

1. `ffmpeg` + `h264_nvenc`（`-rc constqp -qp <auto> -spatial-aq 1 -temporal-aq 1`）
2. `ffmpeg` + `hevc_nvenc`（`-rc constqp -qp <auto+2>`，同样开 AQ）
3. `ffmpeg` + `h264_vaapi`（AMD/Intel 硬编，遍历 `/dev/dri/renderD*` 探测；仅 420）
4. `ffmpeg` + `libx264`（`-crf 16`；≥1440p 用 `veryfast`，否则 `medium`）
5. OpenCV `VideoWriter` mp4v
6. OpenCV `VideoWriter` MJPG AVI fallback

`videoEncoder`：`auto`（默认）| `vaapi`（跳过 NVENC）| `software`（跳过全部
硬件编码器）。非 NVIDIA 设备无需任何配置：探测失败的编码器直接跳过。

编码质量自动选择：QP 基准 18（4:4:4 时 15），`fps > 60` 时 +1，4K 及以上
+1（4K120 → 420 qp 20 / 444 qp 17）。请求可用 `videoQp`（0..51）覆盖。

`videoChroma`：`420`（默认）| `444` | `auto`。
- `420`：所有设备硬解、码率减半，但 4:2:0 色度二次采样会抹掉**单像素级
  彩色细节**（深层 hist_eq 的彩色「雪花」区在任何 QP 下都会发灰发糊——
  实测 420 任意 QP ≈ 19 dB，444 ≈ 39–46 dB）。日常导出/流畅播放选它。
- `444`：逐像素色彩保真（母带存档）。此时优先 `hevc_nvenc`（NVDEC 支持
  HEVC 4:4:4 硬解；h264 4:4:4 只能软解），文件明显更大。
- `auto`：final frame 的像素级细节评分 > 10 时自动选 444。

为什么用 constqp：NVENC 的 target-quality VBR（`-cq`）在本机 driver/ffmpeg
组合下**忽略 `-qmax`**（实测 4K120 噪声源 QP 漂到 50），信息量大的深层段落
会被压成块状碎块（长视频「编码烂帧」的根源）。constqp 是 NVENC 唯一真正
限定每帧质量的模式；码率随内容浮动，AQ 把码率倾斜到高纹理区域。

播放注意：4K120 constqp 输出平均码率可达 ~180 Mbps，解码本身很快（实测
NVDEC/软解都 >700 fps），但**导出运行中同一块 GPU 上播放会卡顿**（CUDA
渲染与合成器/解码争抢），且 KDE 默认播放器对 4K120 的呈现管线较弱——建议
用 `mpv --hwdec=auto`，并避免在导出进行时预览成片。

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

- `standard`: 单一路径渲染（perturbation 门限 1e-13，全程 fp64 delta）。
- `fast`: 尽可能全程 fp32。浅层（≤ ~18 oct）用原有 direct fp32 分层；内圈
  半径 < 1e-5（~18.6 oct）起整段交给 **perturbation**，其中半径 ≥ 1e-30 的
  行用 **fp32 delta**（CUDA/AVX-512/AVX2/scalar 各层都有 fp32 内核；RTX 40
  上 strip 迭代实测 ~15x），更深的行自动回落 fp64 delta——整条 strip 只在
  一个固定全局行发生一次 fp32→fp64 切换。唯一的 final frame 也在
  1e-30 ≤ scale < 1e-13 时用 `perturb-auto-fp32` 渲染。engine/scalar 字段
  会带上 `+fp32` / `perturbation_fp32+fp64` 标记。

相关参数：

- `lnMapScalar`（已废弃：UI 不再提供，后端仍接受但 fast 模式自动分层）
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
- `ln_map_equalization`（segmented `hist_eq`/`bands`/`frontier` 且无法复用 preview stats 时的全局统计 pass）
- `ln_map_render`（真正存在单独统计 pass 时的实际条带渲染 pass）
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
