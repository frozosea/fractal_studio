# Testing / 测试

这份文档记录当前项目可用的构建、测试和手动验证流程。

## Backend Build / 后端构建

```bash
cmake -S backend -B runtime/build -DCMAKE_BUILD_TYPE=Release
cmake --build runtime/build -j
```

等价 Makefile target：

```bash
make backend-build
```

CUDA 是可选能力。CMake 找不到 CUDA 时会禁用 GPU kernels，CPU backend 仍可构建。

## Backend Tests / 后端测试

```bash
ctest --test-dir runtime/build --output-on-failure
```

当前 CMake 注册的测试：

- `special_points_smoke`
- `artifact_routes_smoke`
- `ln_map_reuse_smoke`
- `http_range_smoke`
- `job_runner_reconcile_smoke`
- `orbit_program_smoke`
- `hs_orbit_smoke`
- `compute_path_diff`
- `compute_v1_http_contract`（环境中的 Python 可导入 pytest 时）

测试源文件：

- `backend/src/tests/special_points_smoke.cpp`
- `backend/src/tests/artifact_routes_smoke.cpp`
- `backend/src/tests/ln_map_reuse_smoke.cpp`
- `backend/src/tests/http_range_smoke.cpp`
- `backend/src/tests/job_runner_reconcile_smoke.cpp`
- `backend/src/tests/orbit_program_smoke.cpp`
- `backend/src/tests/hs_orbit_smoke.cpp`
- `backend/src/tests/compute_path_diff.cpp`
- `backend/src/tests/compute_v1/`（fixture 启动真实进程并发送 HTTP）

覆盖重点：

- center period expected count
- Misiurewicz `(preperiod=2, period=1)`，根应为 `c = -2`
- 高周期 local center Newton 收敛
- 深 zoom local center search
- viewport sampled search
- 嵌套 artifact 的 run-relative ID、URL 编码和目录穿越拒绝
- ln-map preview/full-strip 复用的精确 center 与生成参数身份校验
- artifact 单 Range、开放尾端、suffix、multi-range 忽略与 If-Range 全量回退
- Compute v1 鉴权、capabilities、预览、异步 run、轮询、取消、manifest、SHA-256 和 Range
- 每类持久产物的实际 HTTP 生命周期与 kernel hardware telemetry
- 安全 DSL/Orbit sequence、严格逃逸、ln-map/zoom Orbit hash 复用一致性
- 能力注册表的每个 kind 都必须在 `compute_v1_jobs.md` 恰好有一个任务章节；公共端点和 Worker 安全不变量也有文档覆盖检查
- 从零调用手册必须覆盖 Key、workload、DSL/sequence、transition 和硬件证明；参数化 DSL 示例通过真实 HTTP 执行，完整 JSON 示例在文档提交前做语法解析

Compute v1 HTTP 合同可以独立运行：

```bash
python -m pip install -r backend/requirements-test.txt
python -m pytest -q backend/src/tests/compute_v1 \
  --backend-binary=backend/build/fractal_studio_backend \
  --studio-root=.
```

这些测试不是只调用 `.hpp` 的单元样例：fixture 会为测试会话启动实际后端，等待 health，然后通过 Bearer HTTP 验证路由、响应、文件流和取消竞态。每个 `test_*` 独立创建自己的 run/artifact，单个测试只验证一个可命名行为。

## Compute Path Differential Tester / 计算路径对拍器

`compute_path_diff` 会用同一 scalar 的 OpenMP 路径作为基准，对 AVX2、AVX512、CUDA 等可用 map render 路径做 BGR 图像差分。不同 scalar 不直接拿深 zoom 互相比，因为 `fp32`、`fp64`、`fp80`、`fp128`、`fx64` 的舍入模型不同，深度大时结果本来就可能不同。

默认 quick 套件覆盖：

- Mandelbrot escape
- Tricorn / Burning Ship / Buffalo / Perp Buffalo / Celtic Ship 等多 variant 场景
- 10 个 quadratic Mandelbrot-family variant 的完整 map 对拍矩阵
- 同一批 variant 的 Julia 模式对拍矩阵
- Julia escape
- HS-style colorized output: `min_abs`、`max_abs`、`envelope`
- HS rainbow raw-metric coloring (`hs_rainbow`)
- Escape + `hs_rainbow` fallback consistency across CPU/AVX/CUDA
- Escape raw field 对拍：AVX2 覆盖 `fp32` / `fp64`，AVX512 当前覆盖 `fp64`
- Transition renderer direct slice: `theta=0/90°` 对拍普通 map；`theta=-90°/180°` 额外覆盖非零 viewport rotation、精确 center 字符串和 Julia imaginary 的镜像变换
- Transition renderer 非 cardinal slice: milli-degree 输入和 radians 输入对拍，覆盖 escape、HS envelope、pairwise、smooth field
- Rotated transition engine parity: pair/multi × escape/metric 使用 OpenMP 基准对拍 AVX2/CUDA，覆盖 Bird/Celtic Ship fold 和 CUDA `fp32` escape
- 非 cardinal transition 深 zoom：非零 `v`、37° theta、150° viewport rotation 的 Mandelbrot→Bird Julia 边界场景，验证 fp80/可选 fp128 保持高精度 viewport、三角函数与完整 3D orbit，不会退化为 fp64 坐标
- Bird transition volume raw voxel 对拍：OpenMP 语义护栏区分旧 Mask-like fold；AVX2/CUDA 可用时必须实际命中并与 OpenMP fp32 对拍
- OpenMP-only HS/scalar fallback smoke: `min_pairwise_dist`、smooth field coloring、transcendental variant
- `fp64`、`fp32`、`fx64`，以及 OpenMP-only 的 `fp80` / 可选 `fp128`
- AVX2 / AVX512 / CUDA 可用时自动对拍；不可用时记录 `SKIP`

同精度不同算法的默认对拍策略：

- `openmp/fp64` 对 `avx2/fp64`、`avx512/fp64`、`cuda/fp64`
- `openmp/fp32` 对 `avx2/fp32`、`avx512/fp32`、`cuda/fp32`
- `openmp/fx64` 对 `cuda/fx64`，仅在该场景支持固定点整数路径时启用
- `min_pairwise_dist`、smooth coloring、transcendental variant 目前是 OpenMP-only 分支，quick 套件会渲染并标记 `INFO`，但不会把 fallback 当成失败

Escape raw field 不复用 BGR 图像阈值：测试分别约束 iteration mismatch 与 norm delta，避免系统性 iteration `+1/-1` 被宽松的颜色误差掩盖。运行时具备 AVX2 + FMA 时，每个适用场景都必须实际命中 AVX2 field path，不能以回退或其他 AVX2 BGR 覆盖冒充。

Transition 对拍策略：

- Cardinal transition slices 是严格数学等价测试：`theta=0` 必须等于普通 map `from_variant`，`theta=90°` 必须等于普通 map `to_variant`。
- `theta=-90°/180°` 的翻转 direct slice 也必须等于经过严格镜像变换的普通 map；这些 direct slice 会分别跑 `fp64`、`fp32`、`fx64`，要求逐像素完全一致。
- 非 cardinal transition 使用 milli-degree 参数和 radians 参数互相对拍，防止角度归一化和 HS 着色路径漂移。
- 非零 `rotationDeg` 另以 OpenMP fp64 为基准，对拍 AVX2/CUDA 的 pair/multi、escape/metric；escape 还对拍 CUDA fp32。不可用的硬件路径记录 `SKIP`。
- Bird/Celtic Ship 的虚部 fold 严格使用 `2*abs(x*axis)`；旋转 pair/multi 场景防止 SIMD/CUDA 只对 `axis` 取绝对值。
- 非 cardinal `fp80`/`fp128` 另有深 zoom Julia 场景，使用非零 `v`、非同构 variant 和非零 viewport rotation，要求 orbit 的像素在 fp64 已坍缩时仍可区分。
- Bird volume 使用单迭代、远离 bailout 的构造场景；旧的 `2*x*abs(axis)` fold 会触发语义护栏，AVX2/CUDA 可用时禁止 fallback。

跨 scalar 只跑专门构造的 fp32-equivalent 场景：

- 坐标、scale、Julia 参数都是二进制可精确表示的 dyadic 值。
- iteration 很小，避免不同精度的合法舍入差异扩散。
- colormap 使用只依赖整数 iteration 的 `mod17`。
- 基准是 `openmp/fp32`；`openmp/fp64`、`openmp/fp80`、`openmp/fp128`（若编译可用）和 `openmp/fx64` 必须与它逐像素完全一致。

`fp64`、`fp80`、`fp128` 和 `fx64` 另有直接等值对拍：

- 低迭代 dyadic escape 场景要求 BGR 完全一致。
- 低迭代 dyadic HS 场景覆盖 `min_abs`、`max_abs`、`envelope`，同时比较 colorized BGR 和 `render_map_field` raw field。
- 这些场景以 `openmp/fp64` 为基准，`openmp/fp80`、`openmp/fp128`（若编译可用）和 `openmp/fx64` 必须完全一致。

直接运行：

```bash
./runtime/build/compute_path_diff
```

通过 CTest 运行：

```bash
ctest --test-dir runtime/build -R compute_path_diff --output-on-failure
```

测评机可以打开硬件覆盖要求。对应路径没有实际跑到时，测试会失败：

```bash
FSD_DIFF_EXPECT_AVX2=1 \
FSD_DIFF_EXPECT_AVX512=1 \
FSD_DIFF_EXPECT_CUDA=1 \
ctest --test-dir runtime/build -R compute_path_diff --output-on-failure
```

Hybrid 需要足够大的 workload 才会进入真实 CPU+GPU tile scheduler，默认 quick 套件不跑。测评机可开启慢测：

```bash
FSD_DIFF_INCLUDE_SLOW=1 \
FSD_DIFF_EXPECT_HYBRID=1 \
ctest --test-dir runtime/build -R compute_path_diff --output-on-failure
```

输出字段：

- `actual=<engine>/<scalar>`: 后端实际走到的路径。
- `max`: 单通道最大像素差。
- `mean`: 所有 BGR 通道平均绝对差。
- `bad`: 超过阈值的像素比例。
- `SKIP`: 请求路径不可用或回退。
- `FAIL`: 差异超过阈值，或测评机要求的路径未覆盖。

## Frontend Build Check / 前端构建检查

```bash
cd frontend
npm ci
npm run build
```

这个检查能覆盖 TypeScript/Vite/Vue 编译层面的错误，但不等于视觉回归测试。

## Dev Server Smoke / 本地联调冒烟

```bash
./dev.sh
```

默认地址：

```text
Frontend: http://localhost:5174
Backend: http://localhost:18080
```

Backend smoke URLs：

```text
http://localhost:18080/api/system/check
http://localhost:18080/api/system/hardware
http://localhost:18080/api/system/capabilities
http://localhost:18080/api/runs
```

## Manual Feature Checks / 手动功能检查

### 2D Render

- 打开 Map 页面。
- 切换 variant、metric、colormap。
- 拖拽/缩放，确认旧图不会覆盖新视图。
- 试一次 still export，确认 `runtime/runs/maps/<runId>/map.png` 存在。

### Recurrence Metric

- 在 Map 或 3D 中选择 `min_pairwise_dist`。
- 观察响应中的 `engineUsed` 是否回退到 OpenMP。
- 降低 resolution 后再提高 iterations 或 pairwise cap。

### Special Points

- 在 Map 页面打开 special points panel。
- 执行 viewport search。
- 确认返回点有 `accepted=true`、`actual.period` 和 `compatibleVariants`。
- Runs 页面应能看到 search artifact。

### 3D

- 生成 HS field，确认 viewer 有稳定尺寸且不空白。
- 生成 HS mesh，确认 GLB/STL artifact 可下载。
- 生成 transition voxels，确认 `faceCount > 0`。
- 生成 transition mesh，确认 `vertexCount` 和 `triangleCount` 非零。

### Video

- 先跑 `/api/video/preview` 对应的 UI 预览。
- 再跑完整 export。
- Runs 页面确认 `ln_map.png`、`final_frame.png`、`start_frame.png`、`end_frame.png`、`zoom.mp4`、`video_export.json` 都存在。
- 查看 progress stage 是否从 `queued` 到 `completed`。

## Mobile And Tablet QA / 移动端和平板检查

移动端布局细节见 [frontend.md](frontend.md)。每次动前端布局至少检查：

- phone portrait: `390x844` 或 `412x915`
- phone landscape: `844x390`
- tablet landscape: `1024x768`
- large tablet landscape: `1180x820`

重点：

- 顶部导航不遮挡内容。
- StatusRail 移动端默认折叠。
- Map canvas 可见且可拖拽/缩放。
- 控制区局部滚动，不让整个 app 横向溢出。
- 平板横屏不是桌面三栏老布局。

## Pre-commit Checklist / 提交前建议

后端代码改动：

```bash
make backend-build
ctest --test-dir runtime/build --output-on-failure
```

前端代码改动：

```bash
cd frontend
npm ci
npm run build
```

文档改动：

```bash
git diff --check
```

## Known Gaps / 已知缺口

- 目前没有自动浏览器视觉回归测试。
- 没有 Playwright 断点截图检查，移动端布局仍依赖人工 QA。
- 3D canvas 和 video export 更依赖手动验证，因为产物较大、运行时间较长。
