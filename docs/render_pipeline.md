# Render Pipeline / 二维渲染链路

这份文档说明 2D map、Julia、transition slice、raw field、engine/scalar 选择和自定义公式的实现链路。

## Entry Points / 入口

| Endpoint | Route | Purpose |
|---|---|---|
| `POST /api/map/render` | `mapRenderRoute` | 渲染 PNG artifact，写入 `runtime/runs/<runId>/`。 |
| `POST /api/map/render-inline` | `mapRenderInlineRoute` | 直接返回 RGBA8 frame，用于高频交互预览。 |
| `POST /api/map/preempt` | `mapPreemptRoute` | 标记旧交互请求可取消，避免慢响应覆盖新视图。 |
| `POST /api/map/field` | `mapFieldRoute` | 返回 raw field base64，不写 run/artifact，供前端重配色。 |
| `GET /api/variants` | `variantListRoute` | 列出内置和自定义变体。 |
| `POST /api/variants/compile` | `variantCompileRoute` | 编译并加载自定义公式。 |
| `POST /api/variants/delete` | `variantDeleteRoute` | 删除自定义公式。 |

主要代码：

- `backend/src/api/routes_map.cpp`
- `backend/src/api/routes_variants.cpp`
- `backend/src/compute/map_kernel.hpp`
- `backend/src/compute/map_kernel.cpp`
- `backend/src/compute/map_kernel_avx2.cpp`
- `backend/src/compute/map_kernel_avx512.cpp`
- `backend/src/compute/cuda/map_kernel.cu`
- `backend/src/compute/tile_scheduler.cpp`
- `backend/src/compute/transition_kernel.cpp`
- `backend/src/compute/engine_select.cpp`
- `backend/src/compute/escape_time.hpp`
- `backend/src/compute/colormap.hpp`
- `frontend/src/views/MapView.vue`
- `frontend/src/components/MapCanvas.vue`
- `frontend/src/api.ts`

## Request Flow / 请求流

```text
MapView.vue / MapCanvas.vue
  -> frontend/src/api.ts
  -> /api/map/render or /api/map/render-inline
  -> routes_map.cpp::parseMapRenderInput()
  -> routes_map.cpp::renderMapImage()
  -> compute::render_map() or compute::render_transition()
  -> colormap / image_io / artifact response
```

普通二维渲染走 `compute::render_map()`。如果请求里存在 `transitionTheta` 或 `transitionThetaMilliDeg`，就走 `compute::render_transition()`，生成一个三维 transition space 的二维切片。

`/api/map/render` 创建 run，写 PNG artifact，并返回 `artifactId`、`imagePath`、`engineUsed`、`scalarUsed` 和 effective 参数。`/api/map/render-inline` 不创建 artifact，返回二进制 RGBA8，并通过 `X-FSD-*` headers 暴露耗时、engine、scalar、尺寸和 request id。

## Coordinate Model / 坐标模型

`scale` 表示 viewport 在复平面中的高度。宽度跨度为：

```text
width_span = scale * width / height
```

Mandelbrot-family 模式：

```text
c = pixel coordinate
z0 = 0
```

Julia 模式：

```text
z0 = pixel coordinate
c = juliaRe + juliaIm * i
```

## Variants / 变体

| API name | Display name | Formula |
|---|---|---|
| `mandelbrot` | Mandelbrot | `z^2 + c` |
| `tricorn` | Tricorn / Mandelbar | `conj(z)^2 + c` |
| `burning_ship` | Burning Ship | `(abs(Re z) + abs(Im z)i)^2 + c` |
| `celtic` | Perpendicular Burning Ship | `(Re z + abs(Im z)i)^2 + c` |
| `heart` | Perpendicular Mandelbrot | `(abs(Re z) - Im z i)^2 + c` |
| `buffalo` | Celtic | `z^2 -> abs(Re(z^2)) + Im(z^2)i + c` |
| `perp_buffalo` | Mandelbar Celtic | `z^2 -> abs(Re(z^2)) - Im(z^2)i + c` |
| `celtic_ship` | Buffalo | `z^2 -> abs(Re(z^2)) + abs(Im(z^2))i + c` |
| `mandelceltic` | Perpendicular Buffalo | `(Re+abs(Im)i)^2 -> abs(Re)+Im i + c` |
| `perp_ship` | Perpendicular Celtic | `(abs(Re)+Im i)^2 -> abs(Re)-Im i + c` |
| `sin_z` | `sin(z)+c` | `sin(z) + c` |
| `cos_z` | `cos(z)+c` | `cos(z) + c` |
| `exp_z` | `exp(z)+c` | `exp(z) + c` |
| `sinh_z` | `sinh(z)+c` | `sinh(z) + c` |
| `cosh_z` | `cosh(z)+c` | `cosh(z) + c` |
| `tan_z` | `tan(z)+c` | `tan(z) + c` |

后端仍兼容旧别名，如 `tri`、`boat`、`duck`、`bell`、`fish`、`vase`、`bird`、`mask`、`ship`。

## Metrics / 度量

| Metric | Meaning | Notes |
|---|---|---|
| `escape` | classic escape-time | 支持 smooth coloring。 |
| `min_abs` | `min |z_n|` | HS base field。 |
| `max_abs` | `max |z_n|` | envelope 组件。 |
| `envelope` | `min_abs` 和 `max_abs` 的组合 | 当前用于 field/height 可视化。 |
| `min_pairwise_dist` | `min |z_i - z_j|` | 见 [recurrence_metric.md](recurrence_metric.md)。 |

核心迭代在 `backend/src/compute/escape_time.hpp`。Map kernel 根据 metric 选择不同的 `IterResult` 字段，避免为不需要的字段付费。

## Engine And Scalar / 引擎与标量

| Engine | Scope | Notes |
|---|---|---|
| `openmp` | 所有内置变体、Julia、所有 metric、自定义公式 | 最完整路径。 |
| `avx2` | 前 10 个二次变体，escape/min/max/envelope | 需要 AVX2 + FMA。 |
| `avx512` | 前 10 个二次变体，escape/min/max/envelope | 需要 AVX-512F/DQ。 |
| `cuda` | 前 10 个二次变体，Julia，metric 0..3 | 不支持 `min_pairwise_dist`。 |
| `hybrid` | 大任务的 CPU + CUDA tile 调度 | 实际返回可能是 `hybrid`、`cuda`、`avx2`、`avx512` 或 `openmp`。 |

`scalarType: "auto"` 会根据 viewport 深度选择 `fp32`、`fp64` 或 fixed-point。`scale < 1e-13` 时优先进入 fixed-point 路径；不支持的变体、metric 或 engine 会回退。也可以显式请求 OpenMP-only 的 `fp80`，以及编译器支持 libquadmath 时的 `fp128`。

`min_pairwise_dist` 是 O(N^2) orbit-buffer metric，固定点、SIMD、CUDA 都会受限，详见 [recurrence_metric.md](recurrence_metric.md)。

## Transition Slice / 过渡切片

`transitionTheta` 会把屏幕点 `(u, v)` 嵌入 3D：

```text
x0 = u
y0 = v * cos(theta)
z0 = v * sin(theta)
```

`theta = 0` 对应 `transitionFrom` 平面，`theta = 90deg` 对应 `transitionTo` 平面。`transition_kernel.cpp` 会对精确 cardinal angle 做直接 2D map 快捷路径，避免浮点三角函数漂移。

`transitionFrom` 和 `transitionTo` 只支持前 10 个二次/folded variants，不支持 transcendental 和 custom variants。

## Custom Formula / 自定义公式

自定义公式操作复数 `z` 和 `c`。后端 allowlist：

```text
z c sin cos tan exp log pow sqrt abs conj sinh cosh tanh real imag
```

允许字符和操作符：`+ - * / ^ ( ) . 0-9 a-z _` 和空白。`^` 会在常见形式下展开为 `pow(...)`，例如 `z^3 + c`。

编译链路：

```text
POST /api/variants/compile
  -> routes_variants.cpp
  -> validate formula
  -> generate C++ step_fn
  -> g++ shared library
  -> dlopen / dlsym
  -> variant string custom:<hash>
```

自定义公式只适合本地可信环境，不应该直接暴露给不可信公网用户。

## Debug Checklist / 排障点

- 交互拖动残影或旧图覆盖新图：检查 `requestId`、`preemptKey`、`preemptSeq` 和 `/api/map/preempt`。
- engine 和预期不一致：看响应里的 `engineUsed`、`scalarUsed`，并检查 `backend/src/compute/engine_select.cpp`。
- fixed-point 没有启用：确认 `scale`、variant、metric、engine 是否满足限制。
- custom variant 不生效：确认公式已编译，`variant` 是否为 `custom:<hash>`，后端是否能找到 registry 和 `.so`。
- raw field 配色异常：确认 `/api/map/field` 的 metric 类型，escape 返回 `iterB64`/`finalMagB64`，非 escape 返回 `fieldB64`/`fieldMin`/`fieldMax`。
