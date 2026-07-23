# Compute v1 Job Reference / 任务参考

本文逐项定义 Compute v1 当前注册的 18 个 `kind`。如果尚不清楚应该选择哪个 kind，或不知道如何请求 DSL、sequence 和 transition，请先看[从零调用手册](compute_v1_cookbook.md)。公共包络、鉴权、状态、manifest、错误和下载规则见 [compute_v1_contract.md](compute_v1_contract.md)。这里的默认值是 **Compute v1 当前实际默认值**；Platform 可以施加更严格的产品限制，但不能放宽 Compute 限制。

表中“必需产物”表示 `completed` manifest 必须出现的文件。名称按 artifact 文件名判断，MIME 还应与表中一致。所有数值必须为有限值，除非字段另有说明。

## 1. 公共参数类型

### 1.1 Viewport2D

| 字段 | 类型 | 默认/范围 | 说明 |
|---|---|---|---|
| `centerRe`, `centerIm` | number | kind-specific | double 坐标。 |
| `centerReStr`, `centerImStr` | decimal string | omitted | 存在时覆盖对应 number，用于深 zoom；Platform 应原样保存。 |
| `scale` | number | `3.0`, `>0` | viewport 高度；宽度跨度为 `scale * width / height`。 |
| `viewportAspect` | number | `width/height`, `>0` | map/raw 专用逻辑宽高比。 |
| `rotationDeg` | number | `0` | viewport 内旋转角。非有限输入在部分旧适配器中被归零，Platform 应提前拒绝。 |
| `julia` | boolean | `false` | false: `z0=0,c=pixel`; true: `z0=pixel,c=(juliaRe,juliaIm)`。 |
| `juliaRe`, `juliaIm` | number | `0` | Julia 常量。 |

### 1.2 Formula2D

| 字段 | 类型 | 默认/范围 | 说明 |
|---|---|---|---|
| `variant` | string | `mandelbrot` | 内置 ID；完整列表由项目的 variants 文档/能力配置给出。 |
| `orbitProgram` | object | omitted | 安全 formula/sequence IR；仅 `jobs[].orbitProgram=true` 时允许。存在时是数学事实来源。 |
| `iterations` | integer | kind-specific | 最大迭代。 |
| `bailout` | number | variant default, `>0` | 半径。 |
| `bailoutSq` | number | `bailout²`, `>0` | 若单独提供，则 Compute 反推 `bailout=sqrt(bailoutSq)`。 |
| `metric` | enum | kind-specific | `escape`, `min_abs`, `max_abs`, `envelope`, `min_pairwise_dist`；map 另支持 `mandel_ship_agree`。 |
| `pairwiseCap` | integer | `64`, 1..1,000,000 | `min_pairwise_dist` 的 orbit 缓冲上限。 |
| `colorMap` | enum | `classic_cos` | `classic_cos`, `mod17`, `hsv_wheel`, `tri765`, `grayscale`, `hs_rainbow`, `inferno`, `viridis`, `twilight`, `ember_blue`, `spectral1530`。 |
| `engine` | string | kind-specific | 只可从运行时 `jobs[].engines` 选择。 |
| `scalarType` | string | kind-specific | 只可从运行时 `jobs[].scalars` 选择。 |

未知 built-in variant 的部分旧底层路径会回退 Mandelbrot。商业 Platform 必须用 capabilities/配方校验阻止未知 ID，不能依赖该 legacy 行为。生产配方不得使用 `custom:<hash>` 原生动态编译变体；自定义公式必须使用安全 DSL Orbit Program。

### 1.3 输出和内部字段

Platform payload **不要发送** `background`, `taskType`, `localExport`, `requestId`, `preemptKey`, `preemptSeq`。Compute v1 适配器会为持久任务设置后台执行；其余字段属于旧浏览器/本地导出实现，不是平台合同。

## 2. 二维 map 与 raw field

## kind: map_image

调用模式：preview + persistent。Orbit Program：支持。

payload：

| 字段 | 类型 | 默认/范围 |
|---|---|---|
| Viewport2D/Formula2D | — | `center=(-0.75,0)`, `scale=3`, `variant=mandelbrot`, `metric=escape` |
| `width`, `height` | integer | preview/run 默认 1024×768；各 64..8192 |
| `iterations` | integer | 1024；1..1,000,000 |
| `engine` | enum | `openmp`; 能力集合见 capabilities |
| `scalarType` | enum | `auto` |
| `smooth` | boolean | `false` |
| `colorMode` | enum | `direct`; `direct`, `eq_full`, `eq_center` |
| `cyclesPerOctave` | number | `1.0`; `>0` 且 `<=64` |

Orbit Program 与非 `escape` metric 的组合当前返回 `422`。Orbit 也不能和 legacy `transitionTheta*`/`transitionVariants` 字段组合。二维 transition 不是该商业 kind 的一部分，应使用 transition kind。

preview 返回裸 RGBA8，而不是 JSON：

```http
HTTP/1.1 200 OK
Content-Type: application/octet-stream
X-FSD-Status: completed
X-FSD-Request-Id: ...
X-FSD-Generated-Ms: 12.3
X-FSD-Engine: openmp
X-FSD-Scalar: fp64
X-FSD-Width: 512
X-FSD-Height: 512
X-FSD-Pixel-Format: rgba8
```

body 长度必须严格等于 `width * height * 4`，按行连续，通道顺序 RGBA，每通道 uint8。Platform 预览代理应保留上述元数据，但不得将服务密钥传给浏览器。

persistent 必需产物：

| 文件 | kind | MIME |
|---|---|---|
| `map.png` | `image` | `image/png` |

完成进度必须给出实际 `engine`, `scalar`, `kernelReported=true`。

## kind: raw_field

调用模式：preview only。Orbit Program：支持。字段与 `map_image` 相同，但默认 `width=256`, `height=256`, `engine=auto`，尺寸范围各 1..8192；无 `colorMap/colorMode/smooth` 输出意义。

JSON 响应为合同公共包络下的 `data`：

```json
{
  "schemaVersion": 1,
  "data": {
    "status": "completed",
    "width": 256,
    "height": 256,
    "viewportAspect": 1.0,
    "metric": "escape",
    "generatedMs": 4.2,
    "scalarUsed": "fp64",
    "engineUsed": "openmp",
    "maxIter": 1024,
    "iterB64": "...",
    "finalMagB64": "..."
  }
}
```

二进制字段均按 row-major、little-endian 编码后再 base64：

| 条件 | 字段 | 解码类型/元素数 |
|---|---|---|
| `metric=escape` | `iterB64` | uint32 × W×H |
| `metric=escape` | `finalMagB64` | float32 × W×H；bounded 为 0 |
| escape + Orbit | `orbitClassB64` | uint8 × W×H，同时返回 `escapeAnalysis`, `orbitProgramHash` |
| 非 escape | `fieldB64` | float64 × W×H，同时返回 `fieldMin`, `fieldMax` |

Platform 必须先验证解码后的精确字节长度再使用。

## 3. ln-map 与 zoom

## kind: ln_map

调用模式：persistent。Orbit Program：支持。生成对数极坐标条带和可复用 sidecar。

| 字段 | 类型 | 默认/范围 |
|---|---|---|
| Viewport2D | — | center `(0,0)`；Julia 字段可用；`scale` 不参与条带几何 |
| `width`, `height` | integer | `0,0`；若都 >0，用于推导最低 strip 宽度 |
| `depthOctaves` | number | 40；1..1024 |
| `widthS` | integer | 自动；最终向上取 8 的倍数，128..65536 |
| `qualityPreset` | enum | 4K 为 `balanced`，否则 `high`; `draft/balanced/high/full` |
| `qualityScale` | number | preset 对应 0.35/0.55/0.75/1；`>0..1` |
| `lnMapExtraOctaves` | number | escape 2，其他颜色模式 7；2..16 |
| `variant` | built-in | `mandelbrot`; custom 不允许 |
| `iterations` | integer | 4096；1..10,000,000 |
| `lnMapColorMode` | enum | `escape`; `escape/hist_eq/row_eq/log_lift/bands/frontier`；`colorMode` 是兼容别名 |
| `lnMapCyclesPerOctave` | number | 0.5；`>0..64` |
| `engine` | enum | `auto` |
| `precisionMode` | enum | `standard`; `standard/fast`；`lnMapMode` 是覆盖别名 |
| `scalarType` | enum | `auto`；`lnMapScalar` 是覆盖别名 |
| `fastFp32DepthOctaves` | number | 18；>=0 |
| `fastFp64DepthOctaves` | number | 34；>=0 |
| `fastValidate` | boolean | true |
| `fastValidationBandOctaves` | number | 4；>0 |
| `fastValidationSampleRows` | integer | 5；1..32 |
| `fastValidationSampleCols` | integer | 24；1..256 |
| `fastValidationMaxMismatchRatio` | number | 0.01；0..1 |
| `fastValidationMaxP99IterDelta` | integer | 16；>=0 |
| `fastValidationMaxMeanColorDelta` | number | 8；>=0 |

必需产物：`ln_map.png` (`image/png`, kind `image`) 与 `ln_map.json` (`application/json`, kind `report`)。sidecar 包含最终 width/height、中心、深度、颜色、实际 engine/scalar/precision、generation identity；Orbit 请求另含规范化 hash 与逃逸证明。复用时必须以 sidecar 身份字段和 Orbit hash 为准。

## kind: video_preview

调用模式：preview only。Orbit Program：支持。它同步生成 start/end、ln-map 和 final frame 并在 Compute 本地登记临时 run/artifacts；不要把这些临时 artifact 当商业 Asset。

基础字段与 `zoom_video` 相同。额外的 `previewWidth`, `previewHeight` 默认按最长边 720 等比缩放，范围 64..2048 且像素数不超过 1920×1080；`previewLnMapWidthS` 默认 512。当前声明的实际预览 kernel 为 OpenMP/fp64；请求其他值不得被 Platform 解释为硬件承诺。

`data` 至少包含 `runId`, `status=completed`、start/end/final/ln-map artifact IDs、`frameCount`, `fps`, `durationSec`, `depthOctaves`, preview/output 尺寸、实际引擎/标量和生成耗时。图片必须通过私有 artifact 端点读取；`data` 中 `/api/artifacts/*` legacy URL 不得传给浏览器。

## kind: zoom_video

调用模式：persistent。Orbit Program：支持。一次任务负责生成/复用 ln-map、起止预览、最终帧、MP4 和报告。

核心字段：

| 字段 | 类型 | 默认/范围 |
|---|---|---|
| Viewport2D | — | center `(0,0)`, Julia 可用 |
| `width`, `height` | integer | 720×720；各 128..8192，面积 <= 7680×4320 |
| `iterations` | integer | 2048；1..10,000,000 |
| `fps` | integer | 30；1..120 |
| `depthOctaves` | number | 20；0.05..1024 |
| `targetScale` | number | omitted；>0，提供后覆盖 depth 推导 |
| `secondsPerOctave` | number | 0.4；`>0..60` |
| `durationSec` | number | omitted；与 depth 一起反推 speed；总时长 <=3600 秒 |
| `qualityPreset/qualityScale/widthS` | — | 与 ln-map；widthS 最终 128..65536 |
| `lnMapExtraOctaves` | number | escape 2，其他 7；2..16 |
| `lnMapMaxSegmentHeight` | integer | 8192；512..32759，必须大于 extra padding |
| `lnMapRunId` | string | omitted；复用已完成 ln-map run |
| `lnMapStatsRunId` | string | omitted；统计/equalization 复用；`lnMapPreviewRunId` 为兼容别名 |
| `lnMapColorMode` | enum | escape；同 ln_map |
| `lnMapMode`, `lnMapEngine`, `lnMapScalar` | string | standard/auto/auto |
| `lnMapFast*` | — | 与 ln_map 同义，字段前缀为 `lnMapFast...` |
| `lnMapCyclesPerOctave` | number | 0.5；`>0..64` |
| `truncateOnInterior` | boolean | true |
| `cudaWarp` | boolean | true；仅控制图像 warp，不证明 fractal kernel 用 GPU |
| `keepIntermediateVideoFiles` | boolean | false |
| `videoEncoder` | enum | `auto`; `auto/vaapi/software`（auto 可尝试 NVENC） |
| `videoChroma` | enum | `420`; `420/444/auto` |
| `videoQp` | integer | `-1` 自动；显式 0..51 |

总帧数至少 2、最多 10,000,000。`lnMapRunId` 复用要求中心、Julia、variant、颜色、迭代、bailout、几何、precision 配置和 Orbit hash 完全兼容，否则任务失败，绝不混合旧 strip 和新最终帧。

必需产物：

- 一个 `video/mp4`（文件名可能因编码路径而异）；
- `start_frame.png`, `end_frame.png`, `final_frame.png`；
- `video_export.json`；
- 一个或多个 `ln_map*.png` 以及主 `ln_map.json`。分段长视频可以产生多个 strip。

平台应按 MIME+kind 接收，不应假定 MP4 的具体 basename；报告和三张关键帧必须存在。

## kind: legacy_zoom_video

调用模式：persistent。Orbit Program：不支持。这是从已有 `ln_map` artifact/sidecar 生成 MP4 的兼容路径。

| 字段 | 类型 | 默认/范围 |
|---|---|---|
| `lnMapArtifactId` | string | **必需**，格式 `<sourceRunId>:ln_map.png` |
| `width`, `height` | integer | 720×720；同视频尺寸限制 |
| `fps` | integer | 30；1..120 |
| `rotationDeg` | number | sidecar 值或 0 |
| `cudaWarp` | boolean | true |

深度、中心、迭代、variant、颜色、Orbit（若源 sidecar 有）均来自不可变 sidecar。请求不能替换这些数学参数。必需产物：一个 `video/mp4`。新 Platform 流程优先使用 `zoom_video`；只有迁移历史 ln-map 时使用本 kind。

## 4. Axis transition 视频

transition 是轴耦合动力系统，不是公式输出的线性 blend；它不接受 `orbitProgram`。当前只支持 pair，`transitionMode=multi`、`transitionVariants` 或 `transitionWeights` 会明确 `400`，直到动画语义完成。

### TransitionVideoPayload

| 字段 | 类型 | 默认/范围 |
|---|---|---|
| Viewport2D | — | center `(0,0)`, scale 3, Julia 可用 |
| `transitionFrom` | quadratic variant | `mandelbrot` |
| `transitionTo` | quadratic variant | `tri`（推荐显式发送 `burning_ship`） |
| `animationMode` | enum | `rotation`; `rotation/zoom`；`transitionExportMode` 为兼容别名 |
| `thetaStartDeg`, `thetaEndDeg` | number | 0, 180 |
| `thetaDeg` | number | thetaStart；zoom 模式的固定切片角 |
| `width`, `height` | integer | 720×720；各 128..8192，面积限制同 zoom |
| `iterations` | integer | 2048；1..10,000,000 |
| `fps` | integer | 30；1..120 |
| `durationSec` | number | rotation 默认 6；`>0..3600` |
| zoom fields | — | zoom 模式支持 `depthOctaves/targetScale/secondsPerOctave`，限制同 zoom |
| `metric` | enum | escape；`escape/min_abs/max_abs/envelope` |
| `engine`, `scalarType` | enum | auto, auto |
| `colorMap`, `bailout`, `bailoutSq`, `rotationDeg` | — | 语义同公共字段 |

## kind: transition_video_preview

调用模式：preview only。额外支持 `previewWidth/previewHeight`，规则同 video_preview。`data` 包含 `startFrameArtifactId`, `endFrameArtifactId`, animation 参数、frameCount、preview/output 尺寸和 `generatedMs`。本地会产生 `start_frame.png` 与 `end_frame.png`；这是预览临时 run，不进入商业资产流程。

## kind: transition_video

调用模式：persistent。必需产物：`start_frame.png`, `end_frame.png`、一个 `video/mp4`（rotation 通常为 `transition_rotation.mp4`，zoom 通常为 `transition_zoom.mp4`）和 `transition_export.json`。终态进度必须报告 transition fractal kernel 的实际 engine/scalar；FFmpeg encoder 名是另一项观测，不能替代 kernel 证据。

## 5. HS 高度场与网格

## kind: hs_field

调用模式：persistent。Orbit Program：支持。

| 字段 | 类型 | 默认/范围 |
|---|---|---|
| `centerRe`, `centerIm`, `scale` | number | -0.75, 0, 3 |
| `resolution` | integer | 192；8..4096 |
| `iterations` | integer | 512；1..1,000,000 |
| `heightClamp` | number | 2.0 |
| `variant` | built-in/Orbit | mandelbrot |
| `metric` | enum | min_abs；escape/min_abs/max_abs/envelope/min_pairwise_dist |
| `pairwiseCap` | integer | 64；1..1,000,000 |
| `bailout`, `bailoutSq` | number | variant default；>0 |

实际执行固定为 fp64 OpenMP；Orbit 路径 engine 名为 `openmp_orbit`。必需产物：

- `hs_field.f64`, `application/octet-stream`, row-major little-endian float64，元素数 `resolution²`；
- `hs_field.json`, `application/json`：`schemaVersion=1`, `dataType=float64`, `byteOrder=little_endian`, `shape=[resolution,resolution]`, `fieldMin`, `fieldMax`, `engine`, `scalar`。

## kind: hs_mesh

调用模式：persistent。字段与 `hs_field` 相同，另有：

| 字段 | 类型 | 默认 |
|---|---|---|
| `heightScale` | number | 0.6 |
| `heightClamp` | number | 2.0 |

必需产物：`hs_mesh.glb` (`model/gltf-binary`, kind `mesh`) 与 `hs_mesh.stl` (`application/sla`, kind `stl`)。实际执行固定 fp64 OpenMP/`openmp_orbit`。

## 6. Axis transition 体与网格

共同 payload：

| 字段 | 类型 | 默认/范围 |
|---|---|---|
| `centerX`, `centerY`, `centerZ` | number | 0,0,0 |
| `extent` | number | 2.0；应 >0 |
| `resolution` | integer | kind-specific；4..1024，mesh 实际要求至少 8 |
| `iterations` | integer | kind-specific；1..10,000 |
| `bailout`, `bailoutSq` | number | 2, 4；>0 |
| `transitionFrom`, `transitionTo` | quadratic variant | mandelbrot, burning_ship |
| `transitionLegs` | array | omitted；对象 `{variant,weight}` 或 variant 字符串 |
| `transitionVariants`, `transitionWeights` | arrays | omitted；multi 兼容输入 |
| `engine` | enum | auto；auto/openmp/avx2/cuda |
| `scalarType` | enum | fp32；auto/fp32/fp64 |
| `iso` | number | kind-specific |
| `allowLargeVolume` | boolean | false |

`resolution>=512` 必须显式 `allowLargeVolume=true`。`resolution>=256` 会按 VRAM/内存预算进一步拒绝。高并发时 `transition_volume` 资源锁冲突返回 409。Axis 节点只允许前 10 个 quadratic/folded variants；不接受安全 DSL 或普通 Orbit Program。

## kind: transition_mesh

调用模式：persistent。默认 `resolution` 为普通节点 192、low-end CUDA 128；`iterations=256`, `iso=0.5`。必需产物：`transition_mesh.glb` (`model/gltf-binary`) 与 `transition_mesh.stl` (`application/sla`)。进度阶段通常为 `queued`, `volume`, `marching_cubes`, `completed`。

## kind: transition_voxels

调用模式：persistent。默认 `resolution=128`, `iterations=128`, `iso=0.48`。必需产物：`transition_voxels.stl` (`application/sla`)。Compute v1 manifest 不传 legacy base64 face arrays；Platform 资产只接收 STL。进度阶段通常为 `queued`, `volume`, `voxel_mesh`, `completed`。

## 7. Special points

这些 kind 只适用于二次 Mandelbrot critical orbit，不是通用 Orbit Program。`kind` 同时是请求包络判别字段和 special-point payload 内的点类型字段，两者不要混淆。

### SpecialPointViewport

```json
{
  "centerRe": -0.75,
  "centerIm": 0.0,
  "centerReStr": "-0.75",
  "centerImStr": "0",
  "scale": 3.0,
  "rotationDeg": 0.0,
  "width": 1200,
  "height": 800
}
```

点对象包含 `kind`, `preperiod`, `period`, `re`, `im`, 可选全精度 `reStr/imStr/precBits`、`converged`, `accepted`, `visible`, `fallback`, `residual`, `newtonIterations`, `actual`, variant compatibility 和 `reason`。业务可用性以 `accepted` 为准，不是单看 `converged`。

## kind: special_points_enumerate

调用模式：persistent。

| 字段 | 默认/范围 |
|---|---|
| payload `kind` | `center`; `center/hyperbolic/hyperbolic_center/misiurewicz` |
| `periodMin`, `periodMax` | center 1..8，合法上限 10；misiurewicz 默认 1..4，合法上限 6 |
| `preperiodMin`, `preperiodMax` | 1..4；misiurewicz 合法上限 6 |
| `maxNewtonIter` | 60；1..80 |
| `maxSeedBatches` | 80；1..200 |
| `seedsPerBatch` | 2048；1..10000 |
| `newtonEps`, `classifyEps`, `rootMergeEps` | 1e-13, 1e-10, 1e-9 |
| `includeVariantExistence` | true |
| `includeRejectedDebug` | false |
| `visibleOnly` | false |
| `viewport` | optional |

Misiurewicz 要求 `preperiod+period<=10`，预计点总数不超过 3000。必需产物：`special_points.json` (`application/json`, kind `report`)；顶层包含 `version`, `timestamp`, 原请求、response 和 points。

## kind: special_points_search

调用模式：persistent，始终异步。`viewport` 必需。

| 字段 | 默认/范围 |
|---|---|
| payload `kind` | center |
| `periodMin` | 1 |
| `periodMax` | center 默认 8192 次尝试；最大 period 16384，区间长度 <=8192 |
| `preperiodMin`, `preperiodMax` | 1,4；Misiurewicz 单值 <=4096，总和 <=8192，pair 数 <=16384 |
| `seedBudget` | 2000；正整数，受 solver 资源限制 |
| `maxNewtonIter` | 60；1..80 |
| `newtonEps`, `classifyEps`, `rootMergeEps` | 1e-13, 1e-10, 1e-10 |
| `visibleOnly` | true |
| `includeVariantCompatibility` | true |

深 zoom 自动选择 MPFR 并在点对象中给出精确十进制字符串。必需产物：`special_points_search.json`。失败原因可能在 terminal status 的 `progress.errorMessage`，因此 Platform 必须保存 terminal progress。

## kind: special_points_snap

调用模式：preview only。payload：`period`（或兼容 `requestedPeriod`）默认 1、范围 1..10；`re`/`im`（兼容 `real`/`imag`）默认 0；`maxNewtonIter=60`, `newtonEps=1e-13`, `classifyEps=1e-10`, `rootMergeEps=1e-10`, `includeVariantCompatibility=true`。响应 `data.point` 为最近 center。

## kind: special_points_auto

调用模式：preview only。payload：`k=1`, `p=1`, `pointType` 默认由 k 决定。`hyperbolic` 要求 `k=0`，`misiurewicz` 要求 `k>0`，`p>=1`。响应含 `mode=auto`, `k`, `p`, `count`, `points`。

## kind: special_points_seed

调用模式：preview only。payload：`k=1`, `p=1`, `re=0`, `im=0`，`p>=1`。响应含 `mode=seed`, `k`, `p`, `converged`, `points`。

`special_points_auto/seed` 继承 legacy 行为，会写 Compute 节点本地 special-point 历史。Platform 不得把该本地记录视为产品数据；需要保存的点应写入 PostgreSQL 配方/领域表。

## 8. Benchmark

## kind: benchmark

调用模式：persistent。它校准当前节点的 map-field 执行路径并产生多路径硬件证据。

| 字段 | 类型 | 默认/范围 |
|---|---|---|
| `centerRe`, `centerIm` | number | -0.75, 0 |
| `scale` | number | 1.5；>0 |
| `width`, `height` | integer | 512×512；各 64..2048，总像素 <=4,194,304 |
| `iterations` | integer | 2000；1..100000 |
| `samples` | integer | 3；1..7 |
| `warmup` | integer/boolean | 1；0..3，boolean 映射 0/1 |
| `replaceCache` | boolean | true；false 时合并节点选择缓存 |
| `workload` | string | 小于 900k pixels 为 `interactive`，否则 `batch`；1..64，字符 `[A-Za-z0-9_.-]` |

另有限制 `width * height * iterations <= 8,000,000,000`。节点逐项测量可用候选：OpenMP fp32/fp64/fx64，按运行时加入 AVX2/AVX512/CUDA/hybrid。请求路径发生回退时该候选记为 unavailable，而不是把回退结果伪装成请求路径。

必需产物：`benchmark.json` (`application/json`, kind `report`)。报告含 workload、workUnits、尺寸、迭代、warmup/samples 和每个 candidate 的 requested/actual、available、median `elapsedMs`, `mpixPerSec`, `sampleElapsedMs`, error。manifest 的 `hardwareExecution.mode=multi_path`，验收见主合同。

## 9. 能力与必需产物速查

| kind | preview | persistent | Orbit | completed 必需输出 |
|---|:---:|:---:|:---:|---|
| `map_image` | yes | yes | yes | preview RGBA8；run `map.png` |
| `raw_field` | yes | no | yes | JSON/base64 raw arrays |
| `ln_map` | no | yes | yes | `ln_map.png`, `ln_map.json` |
| `video_preview` | yes | no | yes | JSON + 临时 preview images |
| `zoom_video` | no | yes | yes | MP4, 3 frames, report, ln-map files |
| `legacy_zoom_video` | no | yes | no | MP4 |
| `transition_video_preview` | yes | no | no | start/end PNG |
| `transition_video` | no | yes | no | MP4, start/end PNG, report |
| `hs_field` | no | yes | yes | `.f64`, metadata JSON |
| `hs_mesh` | no | yes | yes | GLB, STL |
| `transition_mesh` | no | yes | no | GLB, STL |
| `transition_voxels` | no | yes | no | STL |
| `special_points_enumerate` | no | yes | no | JSON report |
| `special_points_search` | no | yes | no | JSON report |
| `special_points_auto` | yes | no | no | JSON |
| `special_points_seed` | yes | no | no | JSON |
| `special_points_snap` | yes | no | no | JSON |
| `benchmark` | no | yes | no | `benchmark.json` |
