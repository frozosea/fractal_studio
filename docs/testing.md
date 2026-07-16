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
- `compute_path_diff`

测试源文件：

- `backend/src/tests/special_points_smoke.cpp`
- `backend/src/tests/compute_path_diff.cpp`

覆盖重点：

- center period expected count
- Misiurewicz `(preperiod=2, period=1)`，根应为 `c = -2`
- 高周期 local center Newton 收敛
- 深 zoom local center search
- viewport sampled search

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
- Rotated transition engine parity: pair/multi × escape/metric 使用 OpenMP 基准对拍 AVX2/CUDA，并覆盖 CUDA `fp32` escape
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
