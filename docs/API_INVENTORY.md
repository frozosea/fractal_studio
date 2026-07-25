# Fractal Studio — 完整 API 清单

> 自动扫描生成 | 2026-07-25 | 基于源码 `backend/src/api/` + `platform-backend/app/` + `frontend/src/api.ts`

---

## 架构概览

```
┌──────────────┐       ┌──────────────────┐       ┌─────────────────┐
│   Frontend   │──/api/*──▶  C++ Compute   │◀──/compute/v1/*──│  Platform API   │
│  (Vue/TS)    │       │  (port 18080)     │       │  (port 8000)     │
└──────────────┘       └──────────────────┘       └─────────────────┘
                               │                          │
                          SQLite (本地)            PostgreSQL + Redis
```

**两个后端、两套接口体系：**

| 后端 | 语言 | 端口 | 鉴权 | 用途 |
|------|------|------|------|------|
| C++ Compute Backend | C++20 | 18080 | Bearer Token（`/compute/v1/*` 路径）；Legacy `/api/*` 通过环境开关控制 | 分形渲染计算 |
| Platform Backend | Python/FastAPI | 8000 | Foundation Subject（占位，非用户认证） | 业务编排、作业管理 |

---

## 一、鉴权机制

### 1.1 C++ Compute Backend

**无用户登录系统**。本项目没有任何用户注册/登录、JWT、OAuth、Session Cookie、密码哈希。

- **Compute v1 API** (`/compute/v1/*`)：服务间 Bearer Token 认证
  - 环境变量 `FSD_COMPUTE_SERVICE_KEY` 设置共享密钥
  - 请求头 `Authorization: Bearer <KEY>`
  - 常量时间比较（`constantTimeEqual`，XOR 基防时序攻击）
  - 例外：`GET /compute/v1/health` 无需认证
- **Legacy API** (`/api/*`)：通过 `FSD_ENABLE_LEGACY_API` 环境变量控制（默认开启）
  - 无 Bearer Token 要求

### 1.2 Platform Backend

- **Foundation Subject**（占位模式）
  - 通过 `foundation_routes_enabled` 开关控制，生产环境关闭
  - `foundation_subject_id` 作为 `owner_id` 注入所有请求
  - 资源访问按 `owner_id` + `job_id` 范围限定
  - 非真正的用户认证系统（文档标记为未实现）
- **Idempotency-Key** 请求头：幂等键 8-200 字符，防重复提交

---

## 二、分页机制

### 2.1 C++ Legacy API `/api/runs`

- **方式**：传统 LIMIT/OFFSET
- **查询参数**：`?limit=50&offset=0&module=&status=`
- **默认值**：`limit=50`, `offset=0`
- **数据库兜底上限**：200（`Db::listRuns` 内部 SQL `LIMIT` 最大 200）
- **响应包含**：`totalCount`（总数）、`items`（当前页）

### 2.2 其他涉及分页/限制的端点

| 端点 | 方式 | 默认上限 |
|------|------|----------|
| `GET /api/artifacts` | 无分页（全量扫描） | 文件系统遍历 |
| `GET /api/special-points` | 无分页 | `LIMIT 200` |
| `GET /api/variants` | 无分页 | `LIMIT 200` |
| Outbox Worker | `claim_due_batch(limit)` | 配置 `OUTBOX_BATCH_SIZE` |

---

## 三、C++ Compute Backend — Legacy API (`/api/*`)

### 3.1 系统信息

#### `GET /api/system/check`
**说明**：系统模块可用性检查
**鉴权**：无
**入参**：无
**返回**：
```json
{
  "openmp": true,    // bool — OpenMP 编译状态
  "cuda": false      // bool — CUDA 编译状态
}
```

#### `GET /api/system/hardware`
**说明**：硬件信息
**鉴权**：无
**入参**：无
**返回**：
```json
{
  "cpuModel": "string",
  "cpuLogicalCores": 16,
  "cpuPhysicalCores": 8,
  "memoryTotalMiB": 32768,
  "memoryAvailableMiB": 16384,
  "gpuModel": "string",
  "gpuMemory": "string"
}
```

#### `GET /api/system/capabilities`
**说明**：运行时能力（CPU SIMD、CUDA 等）
**鉴权**：无
**入参**：无
**返回**：`Record<string, any>`（含 cpu 和 cuda 能力详情）

---

### 3.2 地图渲染

#### `POST /api/map/render`
**说明**：渲染分形地图，生成 PNG 产物文件
**鉴权**：Legacy 开关
**入参** (`MapRenderRequest`)：
| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| centerRe | number | -0.75 | 中心实部 |
| centerIm | number | 0.0 | 中心虚部 |
| centerReStr | string? | — | 高精度实部字符串（deep zoom） |
| centerImStr | string? | — | 高精度虚部字符串 |
| scale | number | 3.0 | 复平面高度（complex units） |
| viewportAspect | number? | width/height | 视口宽高比 |
| width | number | 1024 | 渲染宽度 (64..MAX_MAP_DIM) |
| height | number | 768 | 渲染高度 (64..MAX_MAP_DIM) |
| iterations | number | 1024 | 最大迭代次数 (1..1000000) |
| variant | string | "mandelbrot" | 分形变体 (16 种内置 或 "custom:HASH") |
| metric | string | "escape" | 度量 (escape/min_abs/max_abs/envelope/min_pairwise_dist/mandel_ship_agree) |
| colorMap | string | "classic_cos" | 色图 (11 种) |
| smooth | boolean? | false | 对数平滑连续着色 |
| colorMode | string? | "direct" | 着色模式 (direct/eq_full/eq_center) |
| cyclesPerOctave | number? | 1.0 | 均衡模式下频带密度 (0..64) |
| bailout | number? | 由 variant 决定 | 逃逸半径 |
| bailoutSq | number? | bailout² | 逃逸半径平方 |
| pairwiseCap | number? | 64 | pairwise 度量上限 |
| julia | boolean? | false | Julia 模式 |
| juliaRe | number? | 0.0 | Julia 常数实部 |
| juliaIm | number? | 0.0 | Julia 常数虚部 |
| transitionTheta | number? | — | 过渡角度（弧度/度，设置此项则启用 Transition Kernel） |
| transitionThetaMilliDeg | number? | — | 过渡角度（毫度） |
| transitionFrom | string? | "mandelbrot" | 过渡起始变体 |
| transitionTo | string? | "burning_ship" | 过渡目标变体 |
| transitionVariants | string[]? | — | 多变体过渡列表 |
| transitionWeights | number[]? | — | 多变体过渡权重 |
| transitionLegs | TransitionLegInput[]? | — | 带权重的过渡腿 |
| engine | string? | "openmp" | 计算引擎 (auto/openmp/avx2/avx512/cuda/hybrid) |
| scalarType | string? | "auto" | 标量精度 (auto/fp32/fp64/fx64/fp80/fp128) |
| rotationDeg | number? | 0.0 | 旋转角度（度） |
| requestId | string? | — | 请求追踪 ID |
| preemptKey | string? | — | 交互式抢占键 |
| preemptSeq | number? | — | 交互式抢占序列号 |
| taskType | string? | — | 设为 "still_export" 启动后台导出 |
| background | boolean? | false | 后台异步执行 |
| localExport | boolean? | false | 本地导出模式 |

**返回** (`MapRenderResponse`)：
```json
{
  "runId": "uuid-string",
  "requestId": "string",        // 可选
  "status": "queued|running|completed|cancelled|failed",
  "artifactId": "runId:map.png",
  "imagePath": "/api/artifacts/content?artifactId=...",
  "localPath": "/path/on/disk",
  "localExport": false,
  "generatedMs": 1234.5,
  "width": 1024,
  "height": 768,
  "scalarUsed": "fp64",
  "engineUsed": "openmp",
  "effective": {
    "centerRe": -0.75,
    "centerIm": 0.0,
    "scale": 3.0,
    "viewportAspect": 1.333,
    "iterations": 1024,
    "variant": "mandelbrot",
    "metric": "escape",
    "colorMap": "classic_cos",
    "bailout": 2.0,
    "bailoutSq": 4.0,
    "julia": false,
    "juliaRe": 0.0,
    "juliaIm": 0.0,
    "transitionTheta": 0.0,
    "transitionThetaMilliDeg": 0,
    "transitionActive": false,
    "transitionFrom": "",
    "transitionTo": "",
    "rotationDeg": 0.0
  }
}
```

#### `POST /api/map/render-inline`
**说明**：即时渲染，返回原始 RGBA8 像素字节（不写文件）
**鉴权**：Legacy 开关
**入参**：同 `MapRenderRequest`（`taskType` 不允许为 "still_export"）
**返回**：
- HTTP 200：`Content-Type: application/octet-stream`，body 为 RGBA8 像素数据
- HTTP 204：已取消
- 响应头：
  - `X-FSD-Status`: "completed" | "cancelled"
  - `X-FSD-Request-Id`: 请求追踪 ID
  - `X-FSD-Generated-Ms`: 渲染耗时
  - `X-FSD-Engine`: 使用的引擎
  - `X-FSD-Scalar`: 使用的标量
  - `X-FSD-Width`: 宽度
  - `X-FSD-Height`: 高度
  - `X-FSD-Pixel-Format`: "rgba8"

#### `POST /api/map/preempt`
**说明**：抢占正在运行的交互式渲染
**鉴权**：Legacy 开关
**入参**：
| 字段 | 类型 | 说明 |
|------|------|------|
| preemptKey | string | 抢占键 (≤128 字节) |
| preemptSeq | number | 抢占序列号 (≥0) |
**返回**：
```json
{ "status": "ok", "preemptKey": "string", "preemptSeq": 0 }
```

#### `POST /api/map/field`
**说明**：原始场数据（不着色），返回 base64 编码的 metric 值
**鉴权**：Legacy 开关
**入参** (`MapFieldRequest`)：与 `MapRenderRequest` 相同（除去 colorMap/smooth/colorMode 等着色字段）
**返回** (`MapFieldResponse`)：

Escape metric 时：
```json
{
  "status": "completed",
  "requestId": "string",
  "width": 256,
  "height": 256,
  "viewportAspect": 1.0,
  "metric": "escape",
  "maxIter": 1024,
  "iterB64": "base64...",       // uint32[W*H] 迭代次数
  "finalMagB64": "base64...",   // float32[W*H] |z|² at escape
  "orbitClassB64": "base64...", // 可选：轨道分类
  "escapeAnalysis": {},         // 可选：逃逸分析结果
  "orbitProgramHash": "string", // 可选
  "generatedMs": 123.4,
  "scalarUsed": "fp64",
  "engineUsed": "openmp"
}
```

非 Escape metric 时：
```json
{
  "status": "completed",
  "fieldB64": "base64...",  // float64[W*H]
  "fieldMin": 0.0,
  "fieldMax": 1234.5
}
```

#### 交互式场会话 (Interactive Field Sessions)

`POST /api/map/field/session/start` — 启动会话
`POST /api/map/field/session/status` — 查询状态
`POST /api/map/field/session/snapshot` — 获取缩略预览 (RGBA)
`POST /api/map/field/session/result` — 获取最终完整场数据
`POST /api/map/field/session/ack` — 确认接收（释放服务端内存）

**会话输入** (`MapFieldSessionStartRequest`)：
| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| requestId | string (必填) | — | 请求追踪 ID |
| preemptKey | string (必填) | — | 抢占键 (≤128 字节) |
| preemptSeq | number (必填) | — | 抢占序列号 (≥0) |
| slowAfterMs | number | 450 | 慢速阈值 (100..5000) |
| colorMap | string? | "classic_cos" | 预览色图 |
| smooth | boolean? | false | 预览平滑着色 |
| 其余 | 同 MapFieldRequest | — | 渲染参数 |

**会话状态响应** (`MapFieldSessionStatus`)：
```json
{
  "sessionId": "map-field-1",
  "requestId": "string",
  "status": "running|completed|cancelled|failed",
  "state": "running|completed|cancelled|failed",
  "width": 1024, "height": 768,
  "viewportAspect": 1.333,
  "centerRe": -0.75, "centerIm": 0.0,
  "scale": 3.0, "rotationDeg": 0.0,
  "elapsedMs": 500, "slowAfterMs": 450,
  "deadlinePassed": true,
  "presentationPhase": "native_wait|degraded|full",
  "revision": 3, "completedPixels": 524288,
  "totalPixels": 786432, "coverage": 0.667,
  "generatedMs": 450.0,       // 仅 completed
  "scalarUsed": "fp64",       // 仅 completed
  "engineUsed": "openmp",     // 仅 completed
  "error": "string",          // 仅 failed
  "started": true,            // 仅 start 响应
  "resultAcknowledged": false // 仅 ack 响应
}
```

**快照响应** (`MapFieldSessionSnapshot`)：扩展自 `MapFieldSessionStatus`，增加：
```json
{
  "previewWidth": 512, "previewHeight": 512,
  "previewAvailable": true,
  "rgbaB64": "base64..."  // RGBA8 预览像素
}
```

**结果响应** (`MapFieldSessionResult`)：
```json
{
  "sessionId": "map-field-1",
  "state": "completed",
  "status": "completed",
  "width": 1024, "height": 1024,
  "metric": "escape", "maxIter": 1024,
  "generatedMs": 450.0,
  "scalarUsed": "fp64", "engineUsed": "openmp",
  "iterB64": "base64...",
  "finalMagB64": "base64..."
}
```

---

### 3.3 Ln-Map 渲染

#### `POST /api/map/ln`
**说明**：对数映射渲染器（用于 deep zoom 视频导出）
**鉴权**：Legacy 开关
**入参** (`LnMapRequest`)：
| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| centerRe | number | -0.75 | 中心实部 |
| centerIm | number | 0.0 | 中心虚部 |
| centerReStr | string? | — | 高精度字符串 |
| centerImStr | string? | — | 高精度字符串 |
| julia | boolean? | false | Julia 模式 |
| juliaRe | number? | 0.0 | Julia 实部 |
| juliaIm | number? | 0.0 | Julia 虚部 |
| widthS | number | — | S 轴宽度 |
| width | number? | — | 图像宽度 |
| height | number? | — | 图像高度 |
| depthOctaves | number | — | 深度倍频程数 |
| qualityPreset | string? | "balanced" | 质量预设 (draft/balanced/high/full/custom) |
| qualityScale | number? | — | 质量缩放系数 |
| lnMapExtraOctaves | number? | — | 额外倍频程 |
| variant | string? | — | 分形变体 |
| colorMap | string? | — | 色图 |
| lnMapColorMode | string? | — | 着色模式 (escape/hist_eq/row_eq/log_lift/bands/frontier) |
| lnMapCyclesPerOctave | number? | — | 每倍频程周期数 |
| iterations | number? | — | 最大迭代次数 |
| engine | string? | — | 引擎 |
| precisionMode | string? | "standard" | 精度模式 (standard/fast) |
| scalarType | string? | — | 标量类型 |
| fastFp32DepthOctaves | number? | — | fast 模式 fp32 深度 |
| fastFp64DepthOctaves | number? | — | fast 模式 fp64 深度 |
| fastValidate | boolean? | — | fast 模式验证 |
| fastValidationBandOctaves | number? | — | 验证频带 |
| fastValidationSampleRows | number? | — | 验证采样行 |
| fastValidationSampleCols | number? | — | 验证采样列 |
| fastValidationMaxMismatchRatio | number? | — | 最大不匹配率 |
| fastValidationMaxP99IterDelta | number? | — | P99 迭代差 |
| fastValidationMaxMeanColorDelta | number? | — | 平均色差 |

**返回** (`LnMapResponse`)：
```json
{
  "runId": "uuid",
  "status": "completed",
  "artifactId": "runId:ln_map.png",
  "imagePath": "/api/artifacts/content?artifactId=...",
  "widthS": 2048, "heightT": 1024,
  "depthOctaves": 32,
  "engineUsed": "openmp",
  "scalarUsed": "fp64",
  "precisionMode": "standard",
  "lnMapColorMode": "escape",
  "lnMapCyclesPerOctave": 2.0,
  "layerSummary": "string",
  "validationSummary": "string",
  "generatedMs": 5678.9
}
```

---

### 3.4 视频导出

#### `POST /api/video/export`
**说明**：统一导出（ln-map + 终帧 + 缩放视频）
**鉴权**：Legacy 开关
**入参** (`VideoExportRequest`)：
| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| centerRe | number | — | 中心实部 |
| centerIm | number | — | 中心虚部 |
| centerReStr | string? | — | 高精度字符串 |
| centerImStr | string? | — | 高精度字符串 |
| julia | boolean? | false | Julia 模式 |
| juliaRe | number? | 0.0 | Julia 实部 |
| juliaIm | number? | 0.0 | Julia 虚部 |
| variant | string? | "mandelbrot" | 分形变体 |
| colorMap | string? | — | 色图 |
| iterations | number? | — | 最大迭代次数 |
| bailout | number? | — | 逃逸半径 |
| bailoutSq | number? | — | 逃逸半径平方 |
| widthS | number? | — | S 轴宽度 |
| depthOctaves | number? | — | 深度倍频程数 |
| fps | number? | — | 帧率 |
| secondsPerOctave | number? | — | 每倍频程秒数 |
| durationSec | number? | — | 持续时长 |
| targetScale | number? | — | 目标 scale |
| qualityPreset | string? | — | 质量预设 |
| qualityScale | number? | — | 质量缩放 |
| lnMapEngine | string? | — | ln-map 引擎 |
| lnMapMode | string? | — | ln-map 模式 |
| lnMapScalar | string? | — | ln-map 标量 |
| lnMapColorMode | string? | — | ln-map 着色模式 |
| lnMapCyclesPerOctave | number? | — | ln-map 周期 |
| lnMapFastValidate | boolean? | — | 验证 |
| lnMapExtraOctaves | number? | — | 额外倍频程 |
| lnMapMaxSegmentHeight | number? | — | 最大分段高度 |
| lnMapRunId | string? | — | 复用已有 ln-map run |
| lnMapStatsRunId | string? | — | 复用 ln-map 统计 |
| lnMapPreviewRunId | string? | — | 复用 ln-map 预览 |
| cudaWarp | boolean? | — | CUDA 变形 |
| background | boolean? | — | 后台执行 |
| localExport | boolean? | — | 本地导出 |
| width | number? | — | 输出宽度 |
| height | number? | — | 输出高度 |
| rotationDeg | number? | — | 旋转角度 |

**返回** (`VideoExportResponse`)：详见前端 `api.ts` VideoExportResponse 接口，包含 video/lnMap/finalFrame/startFrame/endFrame/report 等多产物引用。

#### `POST /api/video/preview`
**说明**：视频预览（仅首尾帧）
**入参** (`VideoPreviewRequest`)：继承 `VideoExportRequest`，增加：
| previewWidth | number? | — | 预览宽度 |
| previewHeight | number? | — | 预览高度 |
**返回** (`VideoPreviewResponse`)：包含 startFrame、endFrame、outputWidth、outputHeight 等。

#### `POST /api/video/zoom`
**说明**：遗留缩放视频（需要已有 ln-map 产物）
**鉴权**：Legacy 开关
**入参** (`VideoZoomRequest`)：
| 字段 | 类型 | 说明 |
|------|------|------|
| lnMapArtifactId | string (必填) | 已有 ln-map 产物 ID |
| localExport | boolean? | 本地导出 |
| fps | number? | 帧率 |
| durationSec | number? | 时长 |
| secondsPerOctave | number? | 每倍频程秒数 |
| targetScale | number? | 目标 scale |
| width | number? | 宽度 |
| height | number? | 高度 |
| startLnRadius | number? | 起始 ln 半径 |
| depthOctaves | number? | 深度倍频程 |
| cudaWarp | boolean? | CUDA 变形 |
| rotationDeg | number? | 旋转角度 |

**返回** (`VideoZoomResponse`)：包含 videoUrl、downloadUrl、frameCount、fps、durationSec 等。

#### `POST /api/video/transition`
**说明**：过渡视频导出（theta 扫掠，逐帧渲染，无 ln-map）
**入参** (`TransitionVideoExportRequest`)：
| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| animationMode | string? | "rotation" | 动画模式 (rotation/zoom) |
| centerRe | number | — | 中心实部 |
| centerIm | number | — | 中心虚部 |
| centerReStr | string? | — | 高精度字符串 |
| centerImStr | string? | — | 高精度字符串 |
| julia | boolean? | false | Julia 模式 |
| juliaRe | number? | 0.0 | Julia 实部 |
| juliaIm | number? | 0.0 | Julia 虚部 |
| transitionFrom | string? | — | 起始变体 |
| transitionTo | string? | — | 目标变体 |
| colorMap | string? | — | 色图 |
| iterations | number? | — | 迭代次数 |
| bailout | number? | — | 逃逸半径 |
| bailoutSq | number? | — | 逃逸半径平方 |
| scale | number? | — | 复平面高度 |
| thetaStartDeg | number? | — | 起始角度（度） |
| thetaEndDeg | number? | — | 结束角度（度） |
| thetaDeg | number? | — | 固定角度（非动画） |
| depthOctaves | number? | — | 深度倍频程 (zoom 模式) |
| secondsPerOctave | number? | — | 每倍频程秒数 |
| targetScale | number? | — | 目标 scale |
| rotationDeg | number? | — | 旋转角度 |
| durationSec | number? | — | 时长 |
| fps | number? | — | 帧率 |
| metric | string? | — | 度量 |
| engine | string? | — | 引擎 |
| scalarType | string? | — | 标量 |
| background | boolean? | — | 后台 |
| localExport | boolean? | — | 本地导出 |
| width | number? | — | 宽度 |
| height | number? | — | 高度 |

**返回** (`TransitionVideoExportResponse`)：与 VideoExportResponse 类似，增加 thetaStartDeg、thetaEndDeg、transitionFrom、transitionTo。

#### `POST /api/video/transition-preview`
**说明**：过渡视频预览
**入参** (`TransitionVideoPreviewRequest`)：继承 `TransitionVideoExportRequest`，增加 previewWidth/previewHeight
**返回** (`TransitionVideoPreviewResponse`)：包含首尾帧引用

---

### 3.5 3D 网格与体素

#### `POST /api/hs/mesh`
**说明**：高度场曲面网格（HS = Heightfield Surface）
**鉴权**：Legacy 开关
**入参** (`HsMeshRequest`)：
| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| centerRe | number? | -0.75 | 中心实部 |
| centerIm | number? | 0.0 | 中心虚部 |
| scale | number? | 3.0 | 复平面高度 |
| width | number? | — | 宽度 |
| height | number? | — | 高度 |
| resolution | number? | — | 网格分辨率 |
| metric | string? | — | 高度度量 (min_abs/max_abs/envelope/min_pairwise_dist) |
| variant | string? | — | 分形变体 |
| iterations | number? | — | 迭代次数 |
| heightScale | number? | — | 高度缩放 |
| pairwiseCap | number? | — | pairwise 上限 |

**返回** (`MeshResponse`)：
```json
{
  "runId": "uuid",
  "status": "completed",
  "glbArtifactId": "runId:mesh.glb",
  "glbUrl": "/api/artifacts/content?artifactId=...",
  "stlArtifactId": "runId:mesh.stl",
  "stlUrl": "/api/artifacts/content?artifactId=...",
  "vertexCount": 12345,
  "triangleCount": 24567,
  "generatedMs": 1234.5,
  "fieldMs": 1000.0,
  "mcMs": 234.5
}
```

#### `POST /api/hs/field`
**说明**：高度场原始数据（float64[W*H]），供前端自行渲染
**鉴权**：Legacy 开关
**入参** (`HsFieldRequest`)：
| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| centerRe | number? | -0.75 | |
| centerIm | number? | 0.0 | |
| scale | number? | 3.0 | |
| resolution | number? | — | |
| metric | string? | — | (min_abs/max_abs/envelope/min_pairwise_dist) |
| variant | string? | — | |
| iterations | number? | — | |
| bailout | number? | — | |
| bailoutSq | number? | — | |
| heightClamp | number? | — | 高度钳制 |
| pairwiseCap | number? | — | |

**返回** (`HsFieldResponse`)：
```json
{
  "runId": "uuid",
  "status": "completed",
  "width": 512, "height": 512,
  "fieldMin": 0.0, "fieldMax": 1.5,
  "fieldB64": "base64...",   // float64[width*height]
  "generatedMs": 123.4
}
```

#### `POST /api/transition/mesh`
**说明**：3D 过渡网格
**鉴权**：Legacy 开关
**入参** (`TransitionMeshRequest`)：
| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| centerX | number? | — | 3D 中心 X |
| centerY | number? | — | 3D 中心 Y |
| centerZ | number? | — | 3D 中心 Z |
| extent | number? | — | 范围 |
| resolution | number? | — | 分辨率 |
| iso | number? | — | 等值面值 |
| iterations | number? | — | 迭代次数 |
| bailout | number? | — | 逃逸半径 |
| bailoutSq | number? | — | 逃逸半径平方 |
| transitionFrom | string? | — | 起始变体 |
| transitionTo | string? | — | 目标变体 |
| transitionVariants | string[]? | — | 多变体列表 |
| transitionWeights | number[]? | — | 权重列表 |
| transitionLegs | TransitionLegInput[]? | — | 过渡腿 |
| engine | string? | — | 引擎 |

**返回** (`MeshResponse`)：同 hs/mesh

#### `POST /api/transition/voxels`
**说明**：体素网格（Minecraft 风格方块）
**鉴权**：Legacy 开关
**入参** (`TransitionVoxelRequest`)：同 TransitionMeshRequest，但 resolution 默认 64 最大 512
**返回** (`TransitionVoxelResponse`)：
```json
{
  "runId": "uuid",
  "status": "completed",
  "resolution": 64, "extent": 3.0,
  "voxelCount": 4096,
  "faceCount": 24576,
  "generatedMs": 567.8,
  "stlArtifactId": "runId:voxels.stl",
  "stlUrl": "/api/artifacts/content?artifactId=...",
  "posB64": "base64...",   // float32[faceCount*4*3]
  "normB64": "base64...",  // int8[faceCount*3]
  "depthB64": "base64..."  // uint8[faceCount]
}
```

---

### 3.6 特殊点（Special Points）

#### `POST /api/special-points/auto`
**说明**：自动计算特殊点（基于周期 k 和前周期 p 查找所有解）
**鉴权**：Legacy 开关
**入参**：
| 字段 | 类型 | 说明 |
|------|------|------|
| k | number | 前周期 |
| p | number | 周期 |
| pointType | string? | 类型 (center/misiurewicz) |
**返回**：
```json
{
  "mode": "auto",
  "k": 1, "p": 2,
  "count": 3,
  "points": [
    {
      "id": "string", "family": "mandelbrot",
      "pointType": "center", "k": 1, "p": 2,
      "real": -1.0, "imag": 0.0,
      "sourceMode": "auto", "createdAt": "ISO8601"
    }
  ]
}
```

#### `POST /api/special-points/seed`
**说明**：从种子点精炼特殊点
**鉴权**：Legacy 开关
**入参**：
| 字段 | 类型 | 说明 |
|------|------|------|
| k | number | 前周期 |
| p | number | 周期 |
| re | number | 种子实部 |
| im | number | 种子虚部 |
**返回**：
```json
{ "mode": "seed", "converged": true, "points": [...] }
```

#### `GET /api/special-points`
**说明**：列出已存储的特殊点
**鉴权**：Legacy 开关
**入参**：`?family=string`（可选，过滤 family）
**返回**：
```json
{ "items": [{ "id": "...", "family": "mandelbrot", ... }] }
```

#### `POST /api/special-points/enumerate`
**说明**：枚举特殊点（Newton 求解器，后台 job）
**鉴权**：Legacy 开关
**入参** (`SpecialPointEnumRequest`)：
| 字段 | 类型 | 说明 |
|------|------|------|
| kind | "center"\|"misiurewicz" | 类型 |
| periodMin | number? | 最小周期 |
| periodMax | number? | 最大周期 |
| preperiodMin | number? | 最小前周期 |
| preperiodMax | number? | 最大前周期 |
| maxNewtonIter | number? | Newton 迭代上限 |
| maxSeedBatches | number? | 种子批次数 |
| seedsPerBatch | number? | 每批种子数 |
| includeVariantExistence | boolean? | 返回变体存在性 |
| includeRejectedDebug | boolean? | 返回被拒点 |
| visibleOnly | boolean? | 仅可见区域 |
| viewport | SpecialPointViewport? | 视口范围 |

**返回** (`SpecialPointEnumResponse`)：
```json
{
  "runId": "uuid", "complete": true, "status": "completed",
  "acceptedCount": 10, "expectedCount": 12,
  "seedCount": 100, "newtonSuccessCount": 50, "rejectedCount": 40,
  "points": [ SpecialPointEnumResult... ],
  "rejected_debug": [ SpecialPointEnumResult... ],
  "warning": "string",
  "reportArtifactId": "string",
  "reportDownloadUrl": "string"
}
```

**`SpecialPointEnumResult` 结构**：
```json
{
  "id": "string",
  "kind": "center|misiurewicz",
  "preperiod": 1, "period": 2,
  "re": 0.0, "im": 0.0,
  "reStr": "high-precision-string",
  "imStr": "high-precision-string",
  "precBits": 256,
  "converged": true, "success": true,
  "accepted": true, "fallback": false,
  "visible": true,
  "residual": 1e-15,
  "newtonIterations": 5,
  "actual": {
    "kind": "center", "found_repeat": true,
    "is_center": true, "is_misiurewicz": false,
    "preperiod": 1, "period": 2,
    "repeat_error": 1e-15
  },
  "variants": [
    {
      "variant_name": "mandelbrot",
      "exists": true,
      "same_orbit_as_mandelbrot": true,
      "actual_preperiod": 1, "actual_period": 2,
      "repeat_error": 1e-15, "reason": "found"
    }
  ],
  "compatibleVariants": ["mandelbrot", "burning_ship"],
  "reason": "converged"
}
```

#### `POST /api/special-points/search`
**说明**：在当前视口内搜索可见特殊点
**鉴权**：Legacy 开关
**入参** (`SpecialPointSearchRequest`)：
| 字段 | 类型 | 说明 |
|------|------|------|
| preemptKey | string? | 抢占键 |
| preemptSeq | number? | 抢占序列号 |
| kind | string? | 类型 |
| periodMin | number? | 最小周期 |
| periodMax | number? | 最大周期 |
| preperiodMin | number? | 最小前周期 |
| preperiodMax | number? | 最大前周期 |
| seedBudget | number? | 种子预算 |
| maxNewtonIter | number? | Newton 迭代上限 |
| includeVariantCompatibility | boolean? | 变体兼容性 |
| visibleOnly | boolean? | 仅可见 |
| viewport | SpecialPointViewport (必填) | 视口 |

**返回** (`SpecialPointSearchResponse`)：
```json
{
  "runId": "uuid", "status": "completed",
  "sampled": true, "foundAny": true, "noPoint": false,
  "acceptedCount": 5, "fallbackCount": 0,
  "seedCount": 50, "newtonSuccessCount": 20, "rejectedCount": 15,
  "points": [ SpecialPointEnumResult... ],
  "warning": "string",
  "reportArtifactId": "string",
  "reportDownloadUrl": "string"
}
```

#### `GET /api/special-points/results`
**说明**：获取搜索结果
**入参**：`?runId=string`
**返回**：同 `SpecialPointSearchResponse`

#### `POST /api/special-points/snap`
**说明**：精确定位已知特殊点
**鉴权**：Legacy 开关
**入参** (`SpecialPointSnapRequest`)：
| 字段 | 类型 | 说明 |
|------|------|------|
| period | number | 周期 |
| re | number | 近似实部 |
| im | number | 近似虚部 |
| maxNewtonIter | number? | Newton 迭代上限 |
| includeVariantCompatibility | boolean? | 变体兼容性 |
**返回**：
```json
{ "point": SpecialPointEnumResult }
```

---

### 3.7 运行管理

#### `GET /api/runs`
**说明**：分页列出历史运行
**鉴权**：Legacy 开关
**入参**（查询参数）：
| 参数 | 类型 | 默认 | 说明 |
|------|------|------|------|
| limit | number | 50 | 每页条数 |
| offset | number | 0 | 偏移 |
| module | string? | — | 模块过滤（逗号分隔，URL 编码） |
| status | string? | — | 状态过滤 |
**返回**：
```json
{
  "items": [
    {
      "id": "uuid",
      "module": "map-export",
      "status": "completed|queued|running|cancelled|failed",
      "startedAt": 1234567890,
      "finishedAt": 1234567900,
      "outputDir": "/path/to/output",
      "cancelable": false,
      "cancelRequested": false
    }
  ],
  "totalCount": 42,
  "modules": ["map-export", "ln-map", "video-export"]
}
```

#### `GET /api/runs/status`
**说明**：单个运行详情
**鉴权**：Legacy 开关
**入参**：`?runId=string`
**返回** (`RunStatusResponse`)：
```json
{
  "id": "uuid",
  "module": "map-export",
  "status": "completed",
  "startedAt": 1234567890,
  "finishedAt": 1234567900,
  "outputDir": "/path",
  "cancelRequested": false,
  "progress": {
    "taskType": "map_export",
    "stage": "completed|render|png_encode|queued|failed|cancelled",
    "current": 1, "total": 1, "percent": 100.0,
    "engine": "openmp", "scalar": "fp64",
    "elapsedMs": 1234.5,
    "estimatedRemainingMs": null,
    "cancelable": false,
    "resourceLocks": ["cuda_heavy", "cpu_heavy"],
    "kernelReported": true,
    "depthOctave": 5, "totalDepthOctaves": 32,
    "currentFrame": 100, "totalFrames": 300,
    "currentLnMapRow": 512, "totalLnMapRows": 1024,
    "finalFrameEngine": "openmp", "finalFrameScalar": "fp64",
    "lnMapEngine": "openmp", "lnMapScalar": "fp64",
    "lnMapMode": "standard",
    "lnMapColorMode": "escape",
    "lnMapPass": "render",
    "warpMethod": "cuda", "encoder": "libx264",
    "failedStage": "render", "errorMessage": "string",
    "details": {}
  },
  "artifacts": [
    {
      "artifactId": "runId:map.png",
      "name": "map.png", "kind": "image",
      "downloadUrl": "/api/artifacts/download?artifactId=...",
      "contentUrl": "/api/artifacts/content?artifactId=...",
      "localPath": "/path/map.png"
    }
  ]
}
```

#### `GET /api/tasks/active`
**说明**：当前活跃任务
**鉴权**：Legacy 开关
**入参**：无
**返回** (`ActiveTasksResponse`)：
```json
{
  "items": [
    {
      "runId": "uuid",
      "taskType": "map_export",
      "status": "running",
      "stage": "render",
      "engine": "openmp", "scalar": "fp64",
      "startedAt": 1234567890,
      "elapsedMs": 5000,
      "cancelable": true, "cancelRequested": false,
      "progress": { ... }
    }
  ],
  "resourceLocks": [
    {
      "name": "cuda_heavy", "active": 1, "limit": 1,
      "busy": true,
      "activeRunId": "uuid", "taskType": "map_export"
    }
  ]
}
```

#### `POST /api/runs/cancel` 或 `POST /api/runs/{runId}/cancel`
**说明**：取消运行
**鉴权**：Legacy 开关
**入参**：`{ "runId": "string" }` 或路径参数
**返回**：
```json
{
  "runId": "uuid",
  "status": "cancel_requested|completed|failed",
  "accepted": true,
  "cancelRequested": true
}
```

---

### 3.8 产物管理

#### `GET /api/artifacts`
**说明**：列出所有产物（文件系统扫描）
**鉴权**：Legacy 开关
**入参**（查询参数）：
| 参数 | 类型 | 说明 |
|------|------|------|
| kind | string? | 按种类过滤 (image/video/stl/mesh/report/other) |
| runId | string? | 按运行 ID 过滤 |
**返回**：
```json
{
  "items": [
    {
      "artifactId": "runId:map.png",
      "runId": "uuid",
      "name": "map.png",
      "kind": "image",
      "sizeBytes": 123456,
      "downloadPath": "/api/artifacts/download?artifactId=...",
      "contentPath": "/api/artifacts/content?artifactId=...",
      "localPath": "/path/on/disk"
    }
  ]
}
```

#### `GET /api/artifacts/content?artifactId=...`
**说明**：获取产物原始内容（内联查看）
**鉴权**：Legacy 开关
**返回**：二进制流，支持 HTTP Range（`Accept-Ranges: bytes`）

#### `GET /api/artifacts/download?artifactId=...`
**说明**：下载产物（触发浏览器下载）
**鉴权**：Legacy 开关
**返回**：二进制流，响应头 `Content-Disposition: attachment; filename="..."`，支持 HTTP Range

---

### 3.9 自定义变体

#### `GET /api/variants`
**说明**：列出所有变体（内置 + 自定义编译）
**鉴权**：Legacy 开关
**返回** (`VariantListResponse`)：
```json
{
  "builtin": [
    { "variantId": "mandelbrot", "name": "mandelbrot", "builtin": true }
  ],
  "custom": [
    {
      "variantId": "custom:abc123",
      "name": "MyFormula",
      "formula": "z*z + c",
      "bailout": 2.0, "bailoutSq": 4.0,
      "createdAt": "ISO8601",
      "loaded": true
    }
  ]
}
```

#### `POST /api/variants/compile`
**说明**：编译自定义公式（g++ → dlopen）
**鉴权**：Legacy 开关 + `FSD_ENABLE_LEGACY_FORMULA_COMPILER=1`
**入参**：
| 字段 | 类型 | 说明 |
|------|------|------|
| formula | string | 数学公式字符串（字母数字+运算符，沙箱白名单） |
| name | string | 变体名称 |
| bailout | number? | 逃逸半径（可选） |
**安全机制**：字符白名单 + 标识符白名单，拒绝 `; { } # \ " '` 等
**返回** (`VariantCompileResponse`)：
```json
{
  "ok": true,
  "variantId": "custom:abc123",
  "name": "MyFormula",
  "hash": "abc123",
  "bailout": 2.0,
  "bailoutSq": 4.0,
  "cached": false,
  "error": null
}
```

#### `POST /api/variants/delete`
**说明**：删除自定义变体
**入参**：`{ "variantId": "custom:abc123" }`
**返回**：`{ "ok": true }`

---

### 3.10 基准测试

#### `POST /api/benchmark`
**说明**：运行性能基准测试
**鉴权**：Legacy 开关
**入参**：`Record<string, any>`（基准测试配置参数）
**返回**：`Record<string, any>`（基准测试结果）

---

## 四、C++ Compute Backend — Compute v1 API (`/compute/v1/*`)

> 私有 API，由 Platform Backend 调用。**所有端点（除 health 外）需 Bearer Token 认证**。

### 4.1 服务发现

#### `GET /compute/v1/health`
**鉴权**：无（负载均衡器可用）
**返回**：
```json
{
  "schemaVersion": 1,
  "status": "ok",
  "service": "fractal-studio-compute",
  "rendererVersion": "dev|version-string"
}
```

#### `GET /compute/v1/capabilities`
**返回**：
```json
{
  "schemaVersion": 1,
  "rendererVersion": "dev",
  "persistentKinds": ["map_image", "ln_map", "zoom_video", ...],
  "previewKinds": ["map_image", "raw_field", "video_preview", ...],
  "jobs": [
    {
      "kind": "map_image",
      "persistent": true, "preview": true, "orbitProgram": true,
      "variantProfile": "builtin_2d_or_safe_dsl",
      "metrics": ["escape", "min_abs", ...],
      "engines": ["auto", "openmp", "avx2", ...],
      "scalars": ["auto", "fp32", "fp64", ...],
      "outputMediaTypes": ["image/png", "application/octet-stream"]
    }
  ],
  "orbitPrograms": { "formula": true, "sequence": true, ... },
  "orbitCompatibility": { "mapImage": true, "rawField": true, ... },
  "customFormula": { "legacyNativeCompile": false, "safeDsl": true },
  "escapeSemantics": { "certifiedRadius": true, "strictUnverified": true },
  "hardware": {
    "cpu": { "logicalCores": 16, "physicalCores": 8, "openmp": {...}, "avx2": {...}, "avx512": {...} },
    "cuda": { "compiled": true, "runtime": true, "deviceCount": 1, "name": "...", ... }
  }
}
```

### 4.2 预览

#### `POST /compute/v1/previews`
**说明**：同步预览（二进制或 JSON 模式）
**鉴权**：Bearer Token

**入参（通用信封）**：
```json
{
  "schemaVersion": 1,
  "kind": "map_image|raw_field|video_preview|transition_video_preview|special_points_auto|special_points_seed|special_points_snap",
  "payload": { /* 按 kind 不同，参数结构与对应的 legacy API 入参一致 */ }
}
```

**特殊行为**：
- `kind: "map_image"` 且有 `schemaVersion: 1` 时：返回二进制 RGBA8 帧（与 `/api/map/render-inline` 相同格式）
- 其余：返回 JSON 包装
```json
{
  "schemaVersion": 1,
  "data": { /* 对应 legacy 响应 */ }
}
```

**JSON 预览返回示例**（raw_field）：
```json
{
  "schemaVersion": 1,
  "data": {
    "status": "completed",
    "width": 256, "height": 256,
    "iterB64": "...", "finalMagB64": "..."
  }
}
```

### 4.3 持久化运行

#### `POST /compute/v1/runs`
**说明**：创建持久化 Compute Run（后台异步执行）
**鉴权**：Bearer Token

**入参（通用信封 + 幂等键）**：
```json
{
  "schemaVersion": 1,
  "kind": "map_image|ln_map|zoom_video|legacy_zoom_video|transition_video|hs_mesh|hs_field|transition_mesh|transition_voxels|special_points_enumerate|special_points_search|benchmark",
  "idempotencyKey": "unique-string (1..200 字符)",
  "payload": { /* 按 kind 不同，参数结构与对应的 legacy API 入参一致 */ }
}
```

**幂等性**：
- 相同 `idempotencyKey` + 相同 `kind`/`payload`（SHA-256） → 返回缓存结果
- 相同 `idempotencyKey` + 不同 payload → HTTP 409 `IDEMPOTENCY_CONFLICT`

**返回**：
```json
{
  "schemaVersion": 1,
  "data": {
    "computeRunId": "uuid",
    "kind": "map_image",
    "status": "queued",
    "legacyResult": { /* 对应 legacy 响应 */ },
    "effective": { "centerRe": -0.75, "centerIm": 0.0, "scale": 3.0 }
  }
}
```

#### `GET /compute/v1/runs/{runId}`
**说明**：查询 Compute Run 状态
**鉴权**：Bearer Token
**返回**：
```json
{
  "schemaVersion": 1,
  "data": {
    "computeRunId": "uuid",
    "status": "completed|queued|running|cancelled|failed",
    "module": "map-export",
    "startedAt": 1234567890,
    "finishedAt": 1234567900,
    "cancelRequested": false,
    "progress": { /* RunProgress */ },
    "hardwareExecution": {
      "mode": "single_path|multi_path",
      "kernelReported": true,
      "evidenceSource": "kernel_completion_telemetry",
      "paths": [ /* 多路径详情 */ ],
      "elapsedMs": 1234.5,
      "requestedEngine": "auto", "actualEngine": "openmp",
      "requestedScalar": "auto", "actualScalar": "fp64",
      "hardwareClass": "cpu", "runtimeAvailable": true,
      "engineFallback": false, "fallbackReason": null
    },
    "artifacts": [
      {
        "artifactId": "runId:map.png",
        "name": "map.png", "kind": "image"
      }
    ]
  }
}
```

#### `POST /compute/v1/runs/{runId}/cancel`
**说明**：取消 Compute Run
**鉴权**：Bearer Token
**返回**：
```json
{
  "schemaVersion": 1,
  "data": {
    "computeRunId": "uuid",
    "status": "cancel_requested|completed",
    "accepted": true,
    "cancelRequested": true
  }
}
```

#### `GET /compute/v1/runs/{runId}/manifest`
**说明**：获取完成运行的完整产物清单（含 SHA-256 校验和、逃逸分析）
**鉴权**：Bearer Token
**返回**：
```json
{
  "schemaVersion": 1,
  "computeRunId": "uuid",
  "rendererVersion": "dev",
  "recipeHash": "sha256-hex-string",
  "status": "completed",
  "effective": { "engine": "openmp", "scalar": "fp64" },
  "hardwareExecution": { ... },
  "escapeAnalysis": {
    "status": "certified_finite|no_finite_bound|unverified",
    "certifiedRadius": 2.0,
    "reason": "string"
  },
  "artifacts": [
    {
      "artifactId": "runId:map.png",
      "name": "map.png", "kind": "image",
      "mediaType": "image/png",
      "sizeBytes": 123456,
      "sha256": "hex-string",
      "contentPath": "/compute/v1/artifacts?artifactId=..."
    }
  ]
}
```

### 4.4 产物访问

#### `GET /compute/v1/artifacts?artifactId=...`
**说明**：流式获取 Compute Run 产物（与 legacy 格式相同）
**鉴权**：Bearer Token
**安全**：路径包含性检查（防目录穿越）、`/proc/self/fd/N` TOCTOU 防护
**返回**：二进制流，支持 HTTP Range

---

## 五、Platform Backend — 业务 API (`/v1/*`, `/health/*`)

> FastAPI 服务，Port 8000

### 5.1 健康检查

#### `GET /health/live`
**鉴权**：无
**返回**：
```json
{ "status": "ok", "service": "fractal-studio-platform" }
```

#### `GET /health/ready`
**鉴权**：无
**返回**：
```json
{
  "status": "ok",
  "environment": "development|production|test",
  "foundationRoutesEnabled": true
}
```

### 5.2 Studio 预览

#### `POST /v1/studio/preview`
**说明**：透传到 Compute Backend 的预览请求
**鉴权**：Foundation Subject（`foundation_routes_enabled=true` 时可用，否则 404）
**入参**：
```json
{
  "kind": "string (1..80 字符)",
  "payload": { /* 透传至 compute /compute/v1/previews */ }
}
```
**返回**：直接透传 Compute Backend 的 HTTP 响应（包括二进制帧和自定义响应头 `X-FSD-*`）

### 5.3 渲染作业

#### `POST /v1/render-jobs`
**说明**：创建渲染作业
**鉴权**：Foundation Subject（owner_id）+ `Idempotency-Key` 请求头 (8..200 字符)
**入参**：
```json
{
  "kind": "string (1..80 字符)",
  "payload": { /* 透传至 compute /compute/v1/runs */ }
}
```
**请求头**：`Idempotency-Key: string`
**返回** (`DataEnvelope<RenderJobView>`)：
```json
{
  "data": {
    "id": "uuid",
    "kind": "map_image",
    "status": "queued",
    "progress_percent": 0,
    "error_code": null,
    "created_at": "ISO8601",
    "finished_at": null
  }
}
```
**状态码**：202 Accepted

#### `GET /v1/render-jobs/{job_id}`
**说明**：查询渲染作业
**鉴权**：Foundation Subject（owner 范围限定）
**入参**：`job_id` (UUID 路径参数)
**返回** (`DataEnvelope<RenderJobView>`)：
```json
{
  "data": {
    "id": "uuid",
    "kind": "map_image",
    "status": "queued|running|completed|cancelled|failed",
    "progress_percent": 50,
    "error_code": "string|null",
    "created_at": "ISO8601",
    "finished_at": "ISO8601|null"
  }
}
```
**错误**：404（作业不存在或不属于该 owner）

#### `POST /v1/render-jobs/{job_id}/cancel`
**说明**：取消渲染作业
**鉴权**：Foundation Subject（owner 范围限定）+ `Idempotency-Key` 请求头
**入参**：`job_id` (UUID 路径参数)
**返回**：同上 `DataEnvelope<RenderJobView>`
**状态码**：202 Accepted

---

## 六、数据库实体（Platform Backend PostgreSQL）

### 6.1 render_jobs 表
| 列 | 类型 | 说明 |
|----|------|------|
| id | UUID (PK) | 主键 |
| owner_id | UUID (NOT NULL) | 所有者 ID |
| kind | VARCHAR(80) | 作业类型 |
| request_json | JSONB | 原始请求体 |
| status | VARCHAR(40) | queued/running/completed/cancelled/failed |
| idempotency_key | VARCHAR(200) | 幂等键 |
| compute_node_id | VARCHAR(120) | Compute 节点 ID |
| compute_run_id | VARCHAR(200) | Compute Run ID |
| progress_percent | INTEGER | 进度百分比 (0-100) |
| result_manifest_json | JSONB | 结果清单 |
| error_code | VARCHAR(120) | 错误码 |
| error_message | VARCHAR(2000) | 错误信息 |
| cancel_requested_at | TIMESTAMPTZ | 取消请求时间 |
| created_at | TIMESTAMPTZ | 创建时间 |
| updated_at | TIMESTAMPTZ | 更新时间 |
| finished_at | TIMESTAMPTZ | 完成时间 |

**约束**：`UNIQUE(owner_id, idempotency_key)`
**索引**：`(owner_id, created_at)`

### 6.2 quota_reservations 表
| 列 | 类型 | 说明 |
|----|------|------|
| id | UUID (PK) | 主键 |
| user_id | UUID (NOT NULL) | 用户 ID |
| render_job_id | UUID (FK→render_jobs ON DELETE CASCADE) | 关联作业 |
| quota_kind | VARCHAR(80) | 配额类型 |
| units | INTEGER | 配额单位数 |
| status | VARCHAR(30) | reserved/completed/released |
| expires_at | TIMESTAMPTZ | 过期时间 |
| created_at | TIMESTAMPTZ | 创建时间 |

**索引**：`(user_id, status)`

### 6.3 outbox_events 表
| 列 | 类型 | 说明 |
|----|------|------|
| id | UUID (PK) | 主键 |
| event_type | VARCHAR | 事件类型 |
| schema_version | INTEGER | 版本 |
| aggregate_type | VARCHAR | 聚合类型 |
| aggregate_id | VARCHAR | 聚合 ID |
| payload_json | JSONB | 事件负载 |
| idempotency_key | VARCHAR (UNIQUE) | 幂等键 |
| status | VARCHAR | pending/processing/completed/failed |
| available_at | TIMESTAMPTZ | 可处理时间 |
| lease_until | TIMESTAMPTZ | 租约到期 |
| attempt_count | INTEGER | 尝试次数 |
| completed_at | TIMESTAMPTZ | 完成时间 |
| last_error | VARCHAR | 最后错误 |
| created_at | TIMESTAMPTZ | 创建时间 |

**索引**：`ix_outbox_claim`（用于 worker 轮询租约）

---

## 七、C++ Compute Backend SQLite 数据记录

| 结构体 | 对应表 | 关键字段 |
|--------|--------|----------|
| `RunRow` | runs | id, module, status, paramsJson, startedAt, finishedAt, outputDir |
| `ArtifactRow` | artifacts | rowId, runId, kind, path, metaJson |
| `SpecialPointRecord` | special_points | id, family, pointType, k, p, re, im, sourceMode, createdAt |
| `CustomVariantRecord` | custom_variants | hash, name, formula, bailout, soPath, createdAt |
| `ComputeIdempotencyRecord` | compute_idempotency | idempotencyKey, requestHash, responseJson, createdAt |

---

## 八、完整端点总览（55 个端点）

### C++ Legacy API (35 个)
| # | 方法 | 路径 | 鉴权 |
|---|------|------|------|
| 1 | GET | /api/system/check | 无 |
| 2 | GET | /api/system/hardware | 无 |
| 3 | GET | /api/system/capabilities | 无 |
| 4 | POST | /api/map/render | Legacy 开关 |
| 5 | POST | /api/map/render-inline | Legacy 开关 |
| 6 | POST | /api/map/preempt | Legacy 开关 |
| 7 | POST | /api/map/field | Legacy 开关 |
| 8 | POST | /api/map/field/session/start | Legacy 开关 |
| 9 | POST | /api/map/field/session/status | Legacy 开关 |
| 10 | POST | /api/map/field/session/snapshot | Legacy 开关 |
| 11 | POST | /api/map/field/session/result | Legacy 开关 |
| 12 | POST | /api/map/field/session/ack | Legacy 开关 |
| 13 | POST | /api/map/ln | Legacy 开关 |
| 14 | POST | /api/video/export | Legacy 开关 |
| 15 | POST | /api/video/preview | Legacy 开关 |
| 16 | POST | /api/video/zoom | Legacy 开关 |
| 17 | POST | /api/video/transition | Legacy 开关 |
| 18 | POST | /api/video/transition-preview | Legacy 开关 |
| 19 | POST | /api/hs/mesh | Legacy 开关 |
| 20 | POST | /api/hs/field | Legacy 开关 |
| 21 | POST | /api/transition/mesh | Legacy 开关 |
| 22 | POST | /api/transition/voxels | Legacy 开关 |
| 23 | POST | /api/special-points/auto | Legacy 开关 |
| 24 | POST | /api/special-points/seed | Legacy 开关 |
| 25 | GET | /api/special-points | Legacy 开关 |
| 26 | POST | /api/special-points/enumerate | Legacy 开关 |
| 27 | POST | /api/special-points/search | Legacy 开关 |
| 28 | GET | /api/special-points/results | Legacy 开关 |
| 29 | POST | /api/special-points/snap | Legacy 开关 |
| 30 | POST | /api/benchmark | Legacy 开关 |
| 31 | GET | /api/variants | Legacy 开关 |
| 32 | POST | /api/variants/compile | Legacy + 编译器开关 |
| 33 | POST | /api/variants/delete | Legacy 开关 |
| 34 | GET | /api/runs | Legacy 开关 |
| 35 | GET | /api/runs/status | Legacy 开关 |
| 36 | GET | /api/tasks/active | Legacy 开关 |
| 37 | POST | /api/runs/cancel | Legacy 开关 |
| 38 | POST | /api/runs/{runId}/cancel | Legacy 开关 |
| 39 | GET | /api/artifacts | Legacy 开关 |
| 40 | GET | /api/artifacts/content | Legacy 开关 |
| 41 | GET | /api/artifacts/download | Legacy 开关 |

### C++ Compute v1 API (8 个)
| # | 方法 | 路径 | 鉴权 |
|---|------|------|------|
| 42 | GET | /compute/v1/health | 无 |
| 43 | GET | /compute/v1/capabilities | Bearer |
| 44 | POST | /compute/v1/previews | Bearer |
| 45 | POST | /compute/v1/runs | Bearer |
| 46 | GET | /compute/v1/runs/{runId} | Bearer |
| 47 | POST | /compute/v1/runs/{runId}/cancel | Bearer |
| 48 | GET | /compute/v1/runs/{runId}/manifest | Bearer |
| 49 | GET | /compute/v1/artifacts | Bearer |

### Platform Backend API (6 个)
| # | 方法 | 路径 | 鉴权 |
|---|------|------|------|
| 50 | GET | /health/live | 无 |
| 51 | GET | /health/ready | 无 |
| 52 | POST | /v1/studio/preview | Foundation Subject |
| 53 | POST | /v1/render-jobs | Foundation Subject + Idempotency-Key |
| 54 | GET | /v1/render-jobs/{job_id} | Foundation Subject |
| 55 | POST | /v1/render-jobs/{job_id}/cancel | Foundation Subject + Idempotency-Key |

---

## 九、安全措施摘要

| 措施 | 位置 | 说明 |
|------|------|------|
| 常量时间密钥比较 | C++ `http_server.cpp` | XOR 基，防时序攻击 |
| 公式沙箱 | C++ `routes_variants.cpp` | 字符 + 标识符白名单，拒绝 `;{}#\"'` |
| 路径包含性检查 | C++ `http_server.cpp` | `pathIsWithin`，防目录穿越 |
| TOCTOU 防护 | C++ `http_server.cpp` | 通过 `/proc/self/fd/N` 重新验证已打开文件路径 |
| 文件名消毒 | C++ `http_server.cpp` | `safeDownloadName`，仅允许字母数字 `.-_ ` |
| SHA-256 完整性 | C++ `routes_compute_v1.cpp` | 产物哈希校验 + 食谱哈希 |
| 幂等性 | C++ `routes_compute_v1.cpp` + Python `router.py` | 防重复提交 (idempotencyKey) |
| Owner 范围限定 | Python `service.py` | 查询作业时同时匹配 job_id + owner_id |
| 取消令牌 | C++ `job_runner.cpp` | `atomic<bool>` 线程安全取消 |
| 信号处理 | C++ `main.cpp` | SIGSEGV/SIGABRT/SIGBUS/SIGPIPE 安全处理 |
| 密钥不得记录 | Python 文档 | 密钥不可写入 DB/日志/异常 |
| 请求体大小限制 | C++ `http_server.cpp` | Header ≤16MiB, Body ≤1GiB |
| 内存预算 | C++ `routes_map.cpp` | 交互式场会话 ≤512MiB 预留，像素 ≤4096² |

---

## 十、关键发现

1. **无用户认证系统**：整个项目没有实现用户注册、登录、JWT、OAuth、Session、密码哈希。鉴权模型仅为服务间共享密钥。
2. **分页实现简单**：仅 `/api/runs` 使用传统 LIMIT/OFFSET，无游标分页。
3. **两层后端**：C++ 渲染引擎 + Python 业务编排层通过 Bearer Token 通信。
4. **Outbox 模式**：Platform Backend 使用 outbox 模式保证可靠异步作业分发。
5. **Foundation 路由为占位实现**：`foundation_subject_id` 为硬编码 UUID，非动态用户认证。
6. **交互式场会话**：支持增量式大图渲染，客户端可按需获取渐进式预览。
