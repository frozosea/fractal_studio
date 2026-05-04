# Fractal Studio / 分形工作室

Fractal Studio is a local fractal exploration app with a native C++ compute
backend and a Vue 3/Vite frontend.

Fractal Studio 是一个本地运行的交互式分形实验室：后端负责原生 C++ 计算、产物管理和 HTTP API，前端提供地图探索、Julia、3D、视频导出和系统诊断界面。

---

## Current Capabilities / 当前功能

- **2D map explorer / 二维地图探索**: full-frame backend rendering with pan, zoom, iteration, palette, metric, scalar and engine controls.
- **16 built-in variants / 16 个内置变体**: 10 quadratic Mandelbrot-family variants plus 6 complex transcendental variants.
- **Custom formula variants / 自定义公式变体**: formulas such as `z^3 + c` are validated, compiled with `g++`, loaded with `dlopen`, and exposed as `custom:<hash>`.
- **Metrics / 度量**: escape time, `min_abs`, `max_abs`, `envelope`, and `min_pairwise_dist`.
- **Color maps / 配色**: `classic_cos`, `mod17`, `hsv_wheel`, `tri765`, `grayscale`, and `hs_rainbow`.
- **Julia mode / Julia 模式**: pick `c` from the Mandelbrot pane and explore the corresponding Julia set in an independent viewport.
- **Axis transition / 轴向过渡**: render 2D and 3D transitions between the first 10 quadratic variants, including the default Mandelbrot to Burning Ship path.
- **3D view / 三维视图**: HS height fields, GLB/STL export, transition voxel previews, marching-cubes STL generation, and voxel-face STL export.
- **Zoom video export / 缩放视频导出**: ln-map strip rendering, start/end preview frames, unified MP4 export, and legacy ln-map to video generation.
- **Runs and artifacts / 运行记录与产物**: completed runs, progress, PNG/MP4/GLB/STL/report files, and download/content URLs are served from the backend.
- **System page / 系统页面**: CPU/GPU/RAM probe, OpenMP/CUDA checks, runtime capabilities, and engine benchmark cache.

---

## Quick Start / 快速启动

### Backend / 后端

Requirements: CMake 3.18+, a C++20 compiler, OpenCV (`core`, `imgcodecs`,
`imgproc`, `videoio`), sqlite3, and Linux/POSIX runtime libraries. Optional:
OpenMP, CUDA toolkit, `ffmpeg` for MP4 export, and `g++` at runtime for custom
formula compilation.

依赖：CMake 3.18+、C++20 编译器、OpenCV（`core`、`imgcodecs`、`imgproc`、`videoio`）、sqlite3，以及 Linux/POSIX 运行环境。可选：OpenMP、CUDA toolkit、用于 MP4 导出的 `ffmpeg`，以及自定义公式运行时编译所需的 `g++`。

From the project root:

```bash
make backend-build
./runtime/build/fractal_studio_backend 18080
```

Equivalent manual build:

```bash
cmake -S backend -B runtime/build -DCMAKE_BUILD_TYPE=Release
cmake --build runtime/build -j
./runtime/build/fractal_studio_backend 18080
```

The backend listens on `0.0.0.0:18080` by default. The first positional argument
may be a port number; the legacy form `serve 18080` also works.

后端默认监听 `0.0.0.0:18080`。第一个命令行参数可以直接传端口号，也兼容旧写法 `serve 18080`。

### Frontend / 前端

Requirements: Node.js 18+.

```bash
cd frontend
npm install
npm run dev
```

Vite serves the UI on port `5173` and binds to the host configured in
`frontend/vite.config.ts`. The frontend defaults to `http://<current-host>:18080`
for API calls. Override it with:

```bash
VITE_BACKEND_URL=http://localhost:18080 npm run dev
```

Production build:

```bash
cd frontend
npm run build
npm run preview
```

### Utility Targets / 辅助命令

```bash
make verify-system      # basic g++/nvcc check used by this repo
make verify-immutable   # checks legacy directories are unchanged
```

`verify-system` currently requires `nvcc`; it is stricter than a CPU-only build.

`verify-system` 目前会要求 `nvcc` 存在，因此它比纯 CPU 构建的实际要求更严格。

---

## Runtime Data / 运行时数据

- Backend build output: `runtime/build/`
- Run artifacts: `runtime/runs/<runId>/`
- Persistent run DB: `runtime/db/fractal_studio.sqlite3`
- Custom variant registry and shared libraries: currently stored relative to
  the parent repo root as `../fractal_studio.db` and `../runs/custom_variants/`
  when launched from this checkout layout.

后端会在 `runtime/` 下写入构建、运行记录和产物。自定义公式模块当前按后端传入的父级 repo root 存放注册表和 `.so` 缓存。

---

## Engine Support / 引擎支持

Direct API callers should pass `engine: "auto"` when they want runtime selection.
If `engine` is omitted on `POST /api/map/render`, the route default is `openmp`.
The frontend engine selector defaults to `auto`.

直接调用 API 时，如果希望后端自动选择引擎，请显式传入 `engine: "auto"`。`POST /api/map/render` 未传 `engine` 时默认使用 `openmp`；前端选择器默认传 `auto`。

| Engine / 引擎 | Scope / 支持范围 | Scalar / 标量 | Notes / 说明 |
|---|---|---|---|
| `openmp` | all 16 built-ins, Julia, all metrics, custom escape rendering | `fp64`, `fx64`, `q4.59`, `q3.60` where valid | Smooth coloring and transcendental variants use this path. |
| `avx2` | first 10 quadratic variants, Julia, escape/min/max/envelope | `fp64` | Requires AVX2 + FMA; no smooth, pairwise, trig, or custom formulas. |
| `avx512` | first 10 quadratic variants, Julia, escape/min/max/envelope | `fp64` | Requires AVX-512F/DQ; fixed-point falls back to OpenMP. |
| `cuda` | first 10 quadratic variants, Julia, escape/min/max/envelope | `fp64`, fixed-point modes | Built only when CUDA is detected by CMake; runtime errors fall back to CPU. |
| `hybrid` | tiled CPU + CUDA rendering for large work | `fp64`, fixed-point where supported | Uses CPU workers plus a GPU tile worker; may report `hybrid`, `cuda`, `avx2`, `avx512`, or `openmp` depending on actual work done. |

`scalarType: "auto"` selects fixed-point when `scale < 1e-13` and the viewport is representable; otherwise it uses `fp64`. Fixed-point is intended for deep zooms of the first 10 quadratic variants and falls back where unsupported.

`scalarType: "auto"` 在 `scale < 1e-13` 且视窗可表示时启用定点数，否则使用 `fp64`。定点数主要用于前 10 个二次变体的深度缩放，不支持时会回退。

### Precision Experiments / 精度实验

The ln-map `fp32`/`fp64`/`fx64` precision and speed experiment notes are in
[`docs/lnmap_precision_experiments.md`](docs/lnmap_precision_experiments.md).

ln-map 的 `fp32` / `fp64` / `fx64` 精度与速度实验记录见
[`docs/lnmap_precision_experiments.md`](docs/lnmap_precision_experiments.md)。

---

## Built-in Variants / 内置变体

| API name | Display name / 显示名称 | Formula / 公式 |
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

Legacy aliases such as `tri`, `boat`, `duck`, `bell`, `fish`, `vase`, `bird`,
`mask`, and `ship` are still accepted by the backend.

后端仍兼容旧 API 名称，例如 `tri`、`boat`、`duck`、`bell`、`fish`、`vase`、`bird`、`mask`、`ship`。

---

## Custom Formula Syntax / 自定义公式语法

Custom formulas operate on complex `z` and `c`. The backend allowlist accepts:

```text
z c sin cos tan exp log pow sqrt abs conj sinh cosh tanh real imag
```

Operators and characters: `+ - * / ^ ( ) . 0-9 a-z _` and whitespace.
`^` is expanded to `pow(...)` for common cases such as `z^3 + c`.

自定义公式中的 `z` 和 `c` 都是复数。允许的函数和标识符见上方列表；`^` 会在常见形式下转换为 `pow(...)`。

---

## Transition Model / 过渡模型

The transition renderer uses a 3D iteration space. A 2D slice at angle `theta`
maps a screen point `(u, v)` to:

```text
x0 = u
y0 = v * cos(theta)
z0 = v * sin(theta)
```

The default UI transition is Mandelbrot to Burning Ship, but the backend also
accepts `transitionFrom` and `transitionTo` for any of the first 10 quadratic
variants. Transcendental and custom variants are not valid transition axes.

默认前端过渡为 Mandelbrot 到 Burning Ship，但后端可通过 `transitionFrom` / `transitionTo` 在前 10 个二次变体之间切换。超越函数变体和自定义公式不能作为过渡轴。

---

## API Summary / API 摘要

All API routes are served by the native backend. JSON endpoints use `POST` with
a JSON body or `GET` with query parameters.

所有 API 都由 C++ 后端提供。JSON 接口使用带 JSON body 的 `POST` 或带 query 参数的 `GET`。

| Endpoint | Description / 说明 |
|---|---|
| `GET /api/system/check` | Basic OpenMP/CUDA availability. |
| `GET /api/system/hardware` | CPU/GPU/RAM information. |
| `GET /api/system/capabilities` | Compiled/runtime engine capabilities and benchmark cache. |
| `POST /api/benchmark` | Benchmark engine/scalar combinations and update the cache. |
| `POST /api/map/render` | Render a colorized PNG artifact. |
| `POST /api/map/field` | Return raw field arrays as base64 JSON, without writing a run artifact. |
| `POST /api/map/ln` | Render a logarithmic strip PNG for zoom workflows. |
| `POST /api/video/preview` | Render start/end preview frames for a zoom video. |
| `POST /api/video/export` | Render ln-map, final frame, and encoded zoom video in one run. |
| `POST /api/video/zoom` | Generate video from an existing ln-map artifact. |
| `POST /api/hs/field` | Return raw HS height-field data for frontend 3D display. |
| `POST /api/hs/mesh` | Generate HS GLB and STL mesh artifacts. |
| `POST /api/transition/mesh` | Generate a marching-cubes transition GLB/STL mesh. |
| `POST /api/transition/voxels` | Generate transition voxel preview data and voxel-face STL. |
| `POST /api/special-points/auto` | Solve periodic or preperiodic Mandelbrot special points. |
| `POST /api/special-points/seed` | Newton-converge a special point from a seed. |
| `GET /api/special-points` | List persisted special points. |
| `GET /api/variants` | List built-in and custom variants. |
| `POST /api/variants/compile` | Compile and load a custom formula. |
| `POST /api/variants/delete` | Unload and delete a custom formula. |
| `GET /api/runs` | List run history. |
| `GET /api/runs/status` | Read run status, progress, and artifacts. |
| `GET /api/artifacts` | List files under `runtime/runs`. |
| `GET /api/artifacts/content` | Serve artifact content inline. |
| `GET /api/artifacts/download` | Serve artifact as a download. |

### Map Render Example / 地图渲染示例

```json
{
  "centerRe": -0.75,
  "centerIm": 0.0,
  "scale": 3.0,
  "width": 1024,
  "height": 768,
  "iterations": 1024,
  "variant": "mandelbrot",
  "metric": "escape",
  "colorMap": "classic_cos",
  "smooth": false,
  "bailout": 2.0,
  "julia": false,
  "juliaRe": 0.0,
  "juliaIm": 0.0,
  "transitionTheta": null,
  "transitionFrom": "mandelbrot",
  "transitionTo": "burning_ship",
  "engine": "auto",
  "scalarType": "auto"
}
```

Supported `engine` values: `auto`, `openmp`, `avx2`, `avx512`, `cuda`, `hybrid`.

Supported `scalarType` values: `auto`, `fp64`, `fx64`, `q6.57`, `q4.59`, `q3.60`
and their legacy aliases.

---

## Project Layout / 项目结构

```text
fractal_studio/
  Makefile
  scripts/
    check_system_requirements.sh
    check_legacy_immutable.sh
  backend/
    CMakeLists.txt
    src/
      main.cpp
      core/
        http_server.cpp        # socket HTTP server and route dispatch
        job_runner.cpp         # run directories, progress, artifacts
        db.cpp                 # sqlite persistence
        hardware_probe.cpp
        path_guard.cpp
      api/
        routes_map.cpp         # /api/map/render and /api/map/field
        routes_ln.cpp          # /api/map/ln
        routes_video.cpp       # preview/export/zoom video
        routes_mesh.cpp        # HS meshes, transition meshes, voxels
        routes_points.cpp      # Mandelbrot special points
        routes_variants.cpp    # custom formula compile/list/delete
        routes_benchmark.cpp
        routes_runs.cpp
        routes_artifacts.cpp
        routes_modules.cpp     # system check/capabilities
      compute/
        variants.hpp
        escape_time.hpp
        colormap.hpp
        map_kernel.cpp
        map_kernel_avx2.cpp
        map_kernel_avx512.cpp
        tile_scheduler.cpp
        transition_kernel.cpp
        transition_volume.cpp
        transition_volume_avx2.cpp
        marching_cubes.cpp
        image_io.cpp
        mesh_io.cpp
        hs/heightfield_mesh.cpp
        newton/mandelbrot_sp.cpp
        scalar/fx64.hpp
        cuda/
          map_kernel.cu
          transition_volume.cu
      adapters/
        cuda_probe.cpp
        openmp_probe.cpp
      third_party/nlohmann/json.hpp
  frontend/
    package.json
    vite.config.ts
    src/
      main.ts
      App.vue
      router.ts
      api.ts
      i18n.ts
      views/
        MapView.vue
        PointsView.vue
        ThreeDView.vue
        RunsView.vue
        SystemView.vue
      components/
        MapCanvas.vue
        ThreeDViewer.vue
        StatusRail.vue
        SpecialPointList.vue
        NavRail.vue
      assets/
        base.css
        tokens.css
```

---

## License / 许可

MIT License.
